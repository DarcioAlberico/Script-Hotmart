#include <stdlib.h>

#include <curl/curl.h>
#include <jansson.h>
#include <bearssl.h>

#include "errors.h"
#include "query.h"
#include "callbacks.h"
#include "types.h"
#include "utils.h"
#include "sha256.h"
#include "symbols.h"
#include "cacert.h"
#include "m3u8.h"

struct SegmentDownload {
	CURL* handle;
	char* filename;
	FILE* stream;
};

#ifdef WIN32
	#define printf(fmt, args...) wprintf(L##fmt, ##args)
	#define fprintf(file, fmt, args...) fwprintf(file, L##fmt, ##args)
#endif

#if defined(WIN32) && defined(UNICODE)
	FILE* fopen(const char* const filename, const char* const mode) {
		int wcsize = 0;
		
		wcsize = MultiByteToWideChar(CP_UTF8, 0, filename, -1, NULL, 0);
		wchar_t wfilename[wcsize];
		MultiByteToWideChar(CP_UTF8, 0, filename, -1, wfilename, wcsize);
		
		wcsize = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0);
		wchar_t wmode[wcsize];
		MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, wcsize);
		
		return _wfopen(wfilename, wmode);
	};
#endif

static const char JSON_FILE_EXTENSION[] = "json";
static const char MP4_FILE_EXTENSION[] = "mp4";
static const char TS_FILE_EXTENSION[] = "ts";
static const char KEY_FILE_EXTENSION[] = "key";

static const char LOCAL_PLAYLIST_FILENAME[] = "playlist.m3u8";

static const char HTTPS_SCHEME[] = "https://";

static const char APP_CONFIG_DIRECTORY[] = ".config";

static const char HTTP_HEADER_AUTHORIZATION[] = "Authorization";
static const char HTTP_HEADER_REFERER[] = "Referer";
static const char HTTP_HEADER_CLUB[] = "Club";
static const char HTTP_DEFAULT_USER_AGENT[] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/106.0.0.0 Safari/537.36";

static const char HTTP_HEADER_SEPARATOR[] = ": ";

static const char* const HOSTNAMES[] = {
	"api-club.hotmart.com:443:52.72.91.225",
	"api-sec-vlc.hotmart.com:443:52.86.213.242",
	"api.sparkleapp.com.br:443:44.196.224.29",
	"hotmart.s3.amazonaws.com:443:52.217.37.220"
};

static const char HOTMART_CLUB_SUFFIX[] = ".club.hotmart.com";

#define HOTMART_API_CLUB_PREFIX "https://api-club.hotmart.com/hot-club-api/rest/v3"
#define HOTMART_API_SEC_PREFIX "https://api-sec-vlc.hotmart.com"
#define SPARKLEAPP_API_PREFIX "https://api.sparkleapp.com.br"

static const char HOTMART_NAVIGATION_ENDPOINT[] = 
	HOTMART_API_CLUB_PREFIX
	"/navigation";

static const char HOTMART_MEMBERSHIP_ENDPOINT[] = 
	HOTMART_API_CLUB_PREFIX
	"/membership";

static const char HOTMART_PAGE_ENDPOINT[] = 
	HOTMART_API_CLUB_PREFIX
	"/page";

static const char HOTMART_ATTACHMENT_ENDPOINT[] = 
	HOTMART_API_CLUB_PREFIX
	"/attachment";

static const char HOTMART_TOKEN_ENDPOINT[] = 
	SPARKLEAPP_API_PREFIX
	"/oauth/token";

static const char HOTMART_TOKEN_CHECK_ENDPOINT[] = 
	HOTMART_API_SEC_PREFIX
	"/security/oauth/check_token";

#define MAX_INPUT_SIZE 1024

static CURL* curl = NULL;

static int authorize(
	const char* const username,
	const char* const password,
	struct Credentials* credentials
) {
	
	char* user = curl_easy_escape(NULL, username, 0);
	
	if (user == NULL) {
		return UERR_CURL_FAILURE;
	}
	
	char* pass = curl_easy_escape(NULL, password, 0);
	
	if (pass == NULL) {
		return UERR_CURL_FAILURE;
	}
	
	struct Query query = {};
	
	add_parameter(&query, "grant_type", "password");
	add_parameter(&query, "username", user);
	add_parameter(&query, "password", pass);
	
	char* post_fields = NULL;
	const int code = query_stringify(query, &post_fields);
	query_free(&query);
	
	if (code != UERR_SUCCESS) {
		return code;
	}
	
	curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, post_fields);
	
	free(post_fields);
	
	struct String string = {0};
	
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &string);
	curl_easy_setopt(curl, CURLOPT_URL, HOTMART_TOKEN_ENDPOINT);
	
	if (curl_easy_perform(curl) != CURLE_OK) {
		return UERR_CURL_FAILURE;
	}
	
	json_t* tree = json_loads(string.s, 0, NULL);
	
	string_free(&string);
	
	if (tree == NULL) {
		return UERR_JSON_CANNOT_PARSE;
	}
	
	const json_t* obj = json_object_get(tree, "access_token");
	
	if (obj == NULL) {
		json_decref(tree);
		return UERR_JSON_MISSING_REQUIRED_KEY;
	}
	
	if (!json_is_string(obj)) {
		json_decref(tree);
		return UERR_JSON_NON_MATCHING_TYPE;
	}
	
	const char* const access_token = json_string_value(obj);
	
	obj = json_object_get(tree, "refresh_token");
	
	if (obj == NULL) {
		json_decref(tree);
		return UERR_JSON_MISSING_REQUIRED_KEY;
	}
	
	if (!json_is_string(obj)) {
		json_decref(tree);
		return UERR_JSON_NON_MATCHING_TYPE;
	}
	
	const char* const refresh_token = json_string_value(obj);
	
	obj = json_object_get(tree, "expires_in");
	
	if (obj == NULL) {
		json_decref(tree);
		return UERR_JSON_MISSING_REQUIRED_KEY;
	}
	
	if (!json_is_integer(obj)) {
		json_decref(tree);
		return UERR_JSON_NON_MATCHING_TYPE;
	}
	
	const int expires_in = json_integer_value(obj);
	
	credentials->access_token = malloc(strlen(access_token) + 1);
	strcpy(credentials->access_token, access_token);
	
	credentials->refresh_token = malloc(strlen(refresh_token) + 1);
	strcpy(credentials->refresh_token, refresh_token);
	
	credentials->expires_in = expires_in;
	
	json_decref(tree);
	
	return UERR_SUCCESS;
	
}

static int get_resources(
	const struct Credentials* const credentials,
	struct Resources* resources
) {
	
	struct Query query = {0};
	
	add_parameter(&query, "token", credentials->access_token);
	
	char* squery = NULL;
	const int code = query_stringify(query, &squery);
	
	query_free(&query);
	
	if (code != UERR_SUCCESS) {
		return code;
	}
	
	CURLU *cu = curl_url();
	curl_url_set(cu, CURLUPART_URL, HOTMART_TOKEN_CHECK_ENDPOINT, 0);
	curl_url_set(cu, CURLUPART_QUERY, squery, 0);
	
	char* url = NULL;
	curl_url_get(cu, CURLUPART_URL, &url, 0);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	
	curl_url_cleanup(cu);
	curl_free(url);
	free(squery);
	
	struct String string = {0};
	
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &string);
	curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
	
	if (curl_easy_perform(curl) != CURLE_OK) {
		return UERR_CURL_FAILURE;
	}
	
	json_t* tree = json_loads(string.s, 0, NULL);
	
	string_free(&string);
	
	if (tree == NULL) {
		return UERR_JSON_CANNOT_PARSE;
	}
	
	const json_t* obj = json_object_get(tree, "resources");
	
	if (obj == NULL) {
		json_decref(tree);
		return UERR_JSON_MISSING_REQUIRED_KEY;
	}
	
	if (!json_is_array(obj)) {
		json_decref(tree);
		return UERR_JSON_NON_MATCHING_TYPE;
	}
	
	char authorization[6 + strlen(SPACE) + strlen(credentials->access_token) + 1];
	strcpy(authorization, "Bearer");
	strcat(authorization, SPACE);
	strcat(authorization, credentials->access_token);
	
	size_t index = 0;
	json_t *item = NULL;
	const size_t array_size = json_array_size(obj);
	
	curl_easy_setopt(curl, CURLOPT_URL, HOTMART_MEMBERSHIP_ENDPOINT);
	
	resources->size = sizeof(struct Resource) * array_size;
	resources->items = malloc(resources->size);
	
	json_array_foreach(obj, index, item) {
		if (!json_is_object(item)) {
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		const json_t* obj = json_object_get(item, "resource");
		
		if (obj == NULL) {
			json_decref(tree);
			return UERR_JSON_MISSING_REQUIRED_KEY;
		}
		
		if (!json_is_object(obj)) {
			json_decref(tree);
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		obj = json_object_get(obj, "subdomain");
		
		if (obj == NULL) {
			json_decref(tree);
			return UERR_JSON_MISSING_REQUIRED_KEY;
		}
		
		if (!json_is_string(obj)) {
			json_decref(tree);
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		const char* const subdomain = json_string_value(obj);
		
		const char* const headers[][2] = {
			{HTTP_HEADER_AUTHORIZATION, authorization},
			{HTTP_HEADER_CLUB, subdomain}
		};
		
		struct curl_slist* list = NULL;
		
		for (size_t index = 0; index < sizeof(headers) / sizeof(*headers); index++) {
			const char** const header = (const char**) headers[index];
			const char* const key = header[0];
			const char* const value = header[1];
			
			char item[strlen(key) + strlen(HTTP_HEADER_SEPARATOR) + strlen(value) + 1];
			strcpy(item, key);
			strcat(item, HTTP_HEADER_SEPARATOR);
			strcat(item, value);
			
			struct curl_slist* tmp = curl_slist_append(list, item);
			
			if (tmp == NULL) {
				curl_slist_free_all(list);
				return UERR_CURL_FAILURE;
			}
			
			list = tmp;
		}
		
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
		
		if (curl_easy_perform(curl) != CURLE_OK) {
			return UERR_CURL_FAILURE;
		}
		
		curl_slist_free_all(list);
		
		json_t* tree = json_loads(string.s, 0, NULL);
		
		string_free(&string);
		
		if (tree == NULL) {
			return UERR_JSON_CANNOT_PARSE;
		}
		
		obj = json_object_get(tree, "name");
		
		if (obj == NULL) {
			json_decref(tree);
			return UERR_JSON_MISSING_REQUIRED_KEY;
		}
		
		if (!json_is_string(obj)) {
			json_decref(tree);
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		const char* const name = json_string_value(obj);
		
		struct Resource resource = {
			.name = malloc(strlen(name) + 1),
			.subdomain = malloc(strlen(subdomain) + 1)
		};
		
		strcpy(resource.name, name);
		strcpy(resource.subdomain, subdomain);
		
		resources->items[resources->offset++] = resource;
		
		json_decref(tree);
	}
	
	return UERR_SUCCESS;
	
}

static int get_modules(
	const struct Credentials* const credentials,
	struct Resource* resource
) {
	
	char authorization[6 + strlen(SPACE) + strlen(credentials->access_token) + 1];
	strcpy(authorization, "Bearer");
	strcat(authorization, SPACE);
	strcat(authorization, credentials->access_token);
	
	const char* const headers[][2] = {
		{HTTP_HEADER_AUTHORIZATION, authorization},
		{HTTP_HEADER_CLUB, resource->subdomain}
	};
	
	struct curl_slist* list = NULL;
	
	for (size_t index = 0; index < sizeof(headers) / sizeof(*headers); index++) {
		const char** const header = (const char**) headers[index];
		const char* const key = header[0];
		const char* const value = header[1];
		
		char item[strlen(key) + strlen(HTTP_HEADER_SEPARATOR) + strlen(value) + 1];
		strcpy(item, key);
		strcat(item, HTTP_HEADER_SEPARATOR);
		strcat(item, value);
		
		struct curl_slist* tmp = curl_slist_append(list, item);
		
		if (tmp == NULL) {
			curl_slist_free_all(list);
			return UERR_CURL_FAILURE;
		}
		
		list = tmp;
	}
	
	struct String string = {0};
	
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &string);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
	curl_easy_setopt(curl, CURLOPT_URL, HOTMART_NAVIGATION_ENDPOINT);
	
	if (curl_easy_perform(curl) != CURLE_OK) {
		return UERR_CURL_FAILURE;
	}
	
	curl_slist_free_all(list);
	
	json_t* tree = json_loads(string.s, 0, NULL);
	
	string_free(&string);
	
	if (tree == NULL) {
		return UERR_JSON_CANNOT_PARSE;
	}
	
	const json_t* obj = json_object_get(tree, "modules");
	
	if (obj == NULL) {
		json_decref(tree);
		return UERR_JSON_MISSING_REQUIRED_KEY;
	}
	
	if (!json_is_array(obj)) {
		json_decref(tree);
		return UERR_JSON_NON_MATCHING_TYPE;
	}
	
	size_t index = 0;
	json_t *item = NULL;
	const size_t array_size = json_array_size(obj);
	
	curl_easy_setopt(curl, CURLOPT_URL, HOTMART_MEMBERSHIP_ENDPOINT);
	
	resource->modules.size = sizeof(struct Module) * array_size;
	resource->modules.items = malloc(resource->modules.size);
	
	json_array_foreach(obj, index, item) {
		if (!json_is_object(item)) {
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		const json_t* obj = json_object_get(item, "id");
		
		if (obj == NULL) {
			json_decref(tree);
			return UERR_JSON_MISSING_REQUIRED_KEY;
		}
		
		if (!json_is_string(obj)) {
			json_decref(tree);
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		const char* const id = json_string_value(obj);
		
		obj = json_object_get(item, "name");
		
		if (obj == NULL) {
			json_decref(tree);
			return UERR_JSON_MISSING_REQUIRED_KEY;
		}
		
		if (!json_is_string(obj)) {
			json_decref(tree);
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		const char* const name = json_string_value(obj);
		
		struct Module module = {
			.id = malloc(strlen(id) + 1),
			.name = malloc(strlen(name) + 1)
		};
		
		strcpy(module.id, id);
		strcpy(module.name, name);
		
		obj = json_object_get(item, "pages");
		
		if (obj == NULL) {
			json_decref(tree);
			return UERR_JSON_MISSING_REQUIRED_KEY;
		}
		
		if (!json_is_array(obj)) {
			json_decref(tree);
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		size_t page_index = 0;
		json_t *page_item = NULL;
		const size_t array_size = json_array_size(obj);
		
		module.pages.size = sizeof(struct Page) * array_size;
		module.pages.items = malloc(module.pages.size);
		
		json_array_foreach(obj, page_index, page_item) {
			if (!json_is_object(page_item)) {
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const json_t* obj = json_object_get(page_item, "hash");
			
			if (obj == NULL) {
				json_decref(tree);
				return UERR_JSON_MISSING_REQUIRED_KEY;
			}
			
			if (!json_is_string(obj)) {
				json_decref(tree);
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const char* const hash = json_string_value(obj);
			
			obj = json_object_get(page_item, "name");
			
			if (obj == NULL) {
				json_decref(tree);
				return UERR_JSON_MISSING_REQUIRED_KEY;
			}
			
			if (!json_is_string(obj)) {
				json_decref(tree);
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const char* const name = json_string_value(obj);
			
			struct Page page = {
				.id = malloc(strlen(hash) + 1),
				.name = malloc(strlen(name) + 1)
			};
			
			strcpy(page.id, hash);
			strcpy(page.name, name);
			
			module.pages.items[module.pages.offset++] = page;
		}
		
		resource->modules.items[resource->modules.offset++] = module;
	}
	
	return UERR_SUCCESS;
	
}

static int get_page(
	const struct Credentials* const credentials,
	const struct Resource* resource,
	struct Page* page
) {
	
	char authorization[6 + strlen(SPACE) + strlen(credentials->access_token) + 1];
	strcpy(authorization, "Bearer");
	strcat(authorization, SPACE);
	strcat(authorization, credentials->access_token);
	
	const char* const headers[][2] = {
		{HTTP_HEADER_AUTHORIZATION, authorization},
		{HTTP_HEADER_CLUB, resource->subdomain},
		{HTTP_HEADER_REFERER, "https://hotmart.com"}
	};
	
	struct curl_slist* list = NULL;
	
	for (size_t index = 0; index < sizeof(headers) / sizeof(*headers); index++) {
		const char** const header = (const char**) headers[index];
		const char* const key = header[0];
		const char* const value = header[1];
		
		char item[strlen(key) + strlen(HTTP_HEADER_SEPARATOR) + strlen(value) + 1];
		strcpy(item, key);
		strcat(item, HTTP_HEADER_SEPARATOR);
		strcat(item, value);
		
		struct curl_slist* tmp = curl_slist_append(list, item);
		
		if (tmp == NULL) {
			curl_slist_free_all(list);
			return UERR_CURL_FAILURE;
		}
		
		list = tmp;
	}
	
	struct String string = {0};
	
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &string);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
	
	char url[strlen(HOTMART_PAGE_ENDPOINT) + strlen(SLASH) + strlen(page->id) + 1];
	strcpy(url, HOTMART_PAGE_ENDPOINT);
	strcat(url, SLASH);
	strcat(url, page->id);
	
	curl_easy_setopt(curl, CURLOPT_URL, url);
	
	if (curl_easy_perform(curl) != CURLE_OK) {
		return UERR_CURL_FAILURE;
	}
	
	json_t* tree = json_loads(string.s, 0, NULL);
	
	string_free(&string);
	
	if (tree == NULL) {
		return UERR_JSON_CANNOT_PARSE;
	}
	
	const json_t* obj = json_object_get(tree, "mediasSrc");
	
	if (obj != NULL) {
		if (!json_is_array(obj)) {
			json_decref(tree);
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		size_t index = 0;
		json_t *item = NULL;
		const size_t array_size = json_array_size(obj);
		
		page->medias.size = sizeof(struct Media) * array_size;
		page->medias.items = malloc(page->medias.size);
		
		json_array_foreach(obj, index, item) {
			if (!json_is_object(item)) {
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const json_t* obj = json_object_get(item, "mediaSrcUrl");
			
			if (obj == NULL) {
				json_decref(tree);
				return UERR_JSON_MISSING_REQUIRED_KEY;
			}
			
			if (!json_is_string(obj)) {
				json_decref(tree);
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const char* const media_page = json_string_value(obj);
			
			curl_easy_setopt(curl, CURLOPT_URL, media_page);
			
			if (curl_easy_perform(curl) != CURLE_OK) {
				return UERR_CURL_FAILURE;
			}
			
			const char* const ptr = strstr(string.s, "mediaAssets");
			
			if (ptr == NULL) {
				string_free(&string);
				return UERR_STRSTR_FAILURE;
			}
			
			const char* const start = strstr(ptr, HTTPS_SCHEME);
			const char* const end = strstr(start, QUOTATION_MARK);
			
			size_t size = end - start;
			
			char url[size + 1];
			memcpy(url, start, size);
			url[size] = '\0';
			
			for (size_t index = 0; index < size; index++) {
				char* offset = &url[index];
				
				if (size > (index + 6) && memcmp(offset, "\\u", 2) == 0) {
					const char c1 = from_hex(*(offset + 4));
					const char c2 = from_hex(*(offset + 5));
					
					*offset = (char) ((c1 << 4) | c2);
					memmove(offset + 1, offset + 6, strlen(offset + 6) + 1);
					
					size -= 5;
				}
			}
			
			string_free(&string);
			
			struct Media media = {
				.url = malloc(strlen(url) + 1)
			};
			
			strcpy(media.url, url);
			
			page->medias.items[page->medias.offset++] = media;
		}
		
	}
	
	obj = json_object_get(tree, "attachments");
	
	if (obj != NULL) {
		if (!json_is_array(obj)) {
			json_decref(tree);
			return UERR_JSON_NON_MATCHING_TYPE;
		}
		
		size_t index = 0;
		json_t *item = NULL;
		const size_t array_size = json_array_size(obj);
		
		page->attachments.size = sizeof(struct Attachment) * array_size;
		page->attachments.items = malloc(page->attachments.size);
		
		json_array_foreach(obj, index, item) {
			if (!json_is_object(item)) {
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const json_t* obj = json_object_get(item, "fileMembershipId");
			
			if (obj == NULL) {
				json_decref(tree);
				return UERR_JSON_MISSING_REQUIRED_KEY;
			}
			
			if (!json_is_string(obj)) {
				json_decref(tree);
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const char* const id = json_string_value(obj);
			
			char url[strlen(HOTMART_ATTACHMENT_ENDPOINT) + strlen(SLASH) + strlen(id) + strlen(SLASH) + 8 + 1];
			strcpy(url, HOTMART_ATTACHMENT_ENDPOINT);
			strcat(url, SLASH);
			strcat(url, id);
			strcat(url, SLASH);
			strcat(url, "download");
			
			curl_easy_setopt(curl, CURLOPT_URL, url);
			
			if (curl_easy_perform(curl) != CURLE_OK) {
				return UERR_CURL_FAILURE;
			}
			
			json_t* tree = json_loads(string.s, 0, NULL);
			
			string_free(&string);
			
			if (tree == NULL) {
				return UERR_JSON_CANNOT_PARSE;
			}
			
			obj = json_object_get(tree, "directDownloadUrl");
			
			if (obj == NULL) {
				json_decref(tree);
				return UERR_JSON_MISSING_REQUIRED_KEY;
			}
			
			if (!json_is_string(obj)) {
				json_decref(tree);
				return UERR_JSON_NON_MATCHING_TYPE;
			}
			
			const char* const download_url = json_string_value(obj);
			
			struct Attachment attachment = {
				.url = malloc(strlen(download_url) + 1)
			};
			
			strcpy(attachment.url, download_url);
			
			page->attachments.items[page->attachments.offset++] = attachment;
			
			json_decref(tree);
		}
	}
	
	json_decref(tree);
	
	return UERR_SUCCESS;
	
}

int b() {
	
	#ifdef _WIN32
		SetConsoleOutputCP(CP_UTF8);
		SetConsoleCP(CP_UTF8);
	#endif
	
	if (!directory_exists(APP_CONFIG_DIRECTORY)) {
		fprintf(stderr, "- Diretório de configurações não encontrado, criando-o\r\n");
		
		if (!create_directory(APP_CONFIG_DIRECTORY)) {
			fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
			exit(EXIT_FAILURE);
		}
		/*
		char* fullpath = NULL;
		
		if (!expand_filename(APP_CONFIG_DIRECTORY, &fullpath)) {
			fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
			exit(EXIT_FAILURE);
		}
		*/
		
		printf("+ Diretório de configurações criado com sucesso!\r\n");
		
	}
	
	char username[MAX_INPUT_SIZE + 1] = {'\0'};
	char password[MAX_INPUT_SIZE + 1] = {'\0'};
	
	while (1) {
		printf("> Insira seu usuário: ");
		
		if (fgets(username, sizeof(username), stdin) != NULL && *username != '\n') {
			break;
		}
		
		fprintf(stderr, "- Usuário inválido ou não reconhecido!\r\n");
	}
	
	*strchr(username, '\n') = '\0';
	
	while (1) {
		printf("> Insira sua senha: ");
		
		if (fgets(password, sizeof(password), stdin) != NULL && *password != '\n') {
			break;
		}
		
		fprintf(stderr, "- Senha inválida ou não reconhecida!\r\n");
	}
	
	*strchr(password, '\n') = '\0';
	
	
	curl_global_init(CURL_GLOBAL_ALL);
	curl = curl_easy_init();
	
	if (curl == NULL) {
		return UERR_CURL_FAILURE;
	}
	
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_DOH_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_TCP_FASTOPEN, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, HTTP_DEFAULT_USER_AGENT);
	curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
	
	struct curl_blob blob = {
		.data = (char*) CACERT,
		.len = strlen(CACERT),
		.flags = CURL_BLOB_COPY
	};
	
	curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
	
	struct curl_slist* resolve_list = NULL;
	
	for (size_t index = 0; index < sizeof(HOSTNAMES) / sizeof(*HOSTNAMES); index++) {
		const char* const hostname = HOSTNAMES[index];
		
		struct curl_slist* tmp = curl_slist_append(resolve_list, hostname);
		
		if (tmp == NULL) {
			curl_slist_free_all(resolve_list);
			return UERR_CURL_FAILURE;
		}
		
		resolve_list = tmp;
	}
	
	curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve_list);
	
	int code = 0;
	
	struct Credentials credentials = {0};
	code = authorize(username, password, &credentials);
	
	if (code != UERR_SUCCESS) {
		fprintf(stderr, "- Não foi possível realizar a autenticação!\r\n");
		exit(EXIT_FAILURE);
	}
	
	printf("+ Usuário autenticado com sucesso!\r\n");
	
	char sha256[(br_sha256_SIZE * 2) + 1];
	sha256_digest(username, sha256);
	
	char filename[strlen(APP_CONFIG_DIRECTORY) + strlen(SLASH) + strlen(sha256) + strlen(DOT) + strlen(JSON_FILE_EXTENSION) + 1];
	strcpy(filename, APP_CONFIG_DIRECTORY);
	strcat(filename, SLASH);
	strcat(filename, sha256);
	strcat(filename, DOT);
	strcat(filename, JSON_FILE_EXTENSION);
	
	printf("+ Exportando arquivo de credenciais\r\n");
	
	FILE* file = fopen(filename, "wb");
	
	if (file == NULL) {
		fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
		exit(EXIT_FAILURE);
	}
	
	json_t* tree = json_object();
	json_object_set_new(tree, "username", json_string(username));
	json_object_set_new(tree, "access_token", json_string(credentials.access_token));
	json_object_set_new(tree, "refresh_token", json_string(credentials.refresh_token));
	json_object_set_new(tree, "expires_in", json_string(credentials.refresh_token));
	
	char* buffer = json_dumps(tree, JSON_COMPACT);
	const size_t buffer_size = strlen(buffer);
	
	const size_t wsize = fwrite(buffer, sizeof(*buffer), buffer_size, file);
	
	free(buffer);
	json_decref(tree);
	fclose(file);
	
	if (wsize != buffer_size) {
		fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
		exit(EXIT_FAILURE);
	}
	
	printf("+ Arquivo de credenciais exportado com sucesso!\r\n");
	
	printf("+ Obtendo lista de produtos\r\n");
	
	struct Resources resources = {0};
	code = get_resources(&credentials, &resources);
	
	if (code != UERR_SUCCESS) {
		fprintf(stderr, "- Não foi possível obter a lista de produtos!\r\n");
		exit(EXIT_FAILURE);
	}
	
	printf("+ Selecione o que deseja baixar:\r\n\r\n");
	
	printf("0.\r\nTodos os produtos disponíveis\r\n\r\n");
	
	for (size_t index = 0; index < resources.offset; index++) {
		const struct Resource* resource = &resources.items[index];
		
		printf("%zu. \r\nNome: %s\r\nHomepage: https://%s%s\r\n\r\n", index + 1, resource->name, resource->subdomain, HOTMART_CLUB_SUFFIX);
	}
	
	char answer[4 + 1];
	int value = 0;
	
	while (1) {
		printf("> Digite sua escolha: ");
		
		if (fgets(answer, sizeof(answer), stdin) != NULL && *answer != '\n') {
			*strchr(answer, '\n') = '\0';
			
			if (isnumeric(answer)) {
				value = atoi(answer);
				
				if (value >= 0 && (size_t) value <= resources.offset) {
					break;
				}
			}
		}
		
		fprintf(stderr, "- Opção inválida ou não reconhecida!\r\n");
	}
	
	struct Resource* download_queue[resources.offset];
	
	size_t queue_count = 0;
	
	if (value == 0) {
		for (size_t index = 0; index < sizeof(download_queue) / sizeof(*download_queue); index++) {
			struct Resource* resource = &resources.items[index];
			download_queue[index] = resource;
			
			queue_count++;
		}
	} else {
		struct Resource* resource = &resources.items[value - 1];
		*download_queue = resource;
		
		queue_count++;
	}
	
	char* cwd = NULL;
	
	if (!expand_filename(".", &cwd)) {
		fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
		exit(EXIT_FAILURE);
	}
	
	for (size_t index = 0; index < queue_count; index++) {
		struct Resource* resource = &resources.items[index];
		
		printf("+ Obtendo lista de módulos do produto '%s'\r\n", resource->name);
		
		if (get_modules(&credentials, resource) != UERR_SUCCESS) {
			fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
			exit(EXIT_FAILURE);
		}
		
		char directory[strlen(resource->name) + 1];
		strcpy(directory, resource->name);
		normalize_filename(directory);
		
		char resource_directory[strlen(cwd) + strlen(PATH_SEPARATOR) + strlen(directory) + 1];
		strcpy(resource_directory, cwd);
		strcat(resource_directory, PATH_SEPARATOR);
		strcat(resource_directory, directory);
		
		if (!directory_exists(resource_directory)) {
			fprintf(stderr, "- O diretório '%s' não existe, criando-o\r\n", resource_directory);
			
			if (!create_directory(resource_directory)) {
				fprintf(stderr, "- Ocorreu um erro ao tentar criar o diretório!\r\n");
				exit(EXIT_FAILURE);
			}
		}
		
		for (size_t index = 0; index < resource->modules.offset; index++) {
			struct Module* module = &resource->modules.items[index];
			
			printf("+ Verificando estado do módulo '%s'\r\n", module->name);
			
			char directory[strlen(module->name) + 1];
			strcpy(directory, module->name);
			normalize_filename(directory);
			
			char module_directory[strlen(resource_directory) + strlen(PATH_SEPARATOR) + strlen(directory) + 1];
			strcpy(module_directory, resource_directory);
			strcat(module_directory, PATH_SEPARATOR);
			strcat(module_directory, directory);
			
			if (!directory_exists(module_directory)) {
				fprintf(stderr, "- O diretório '%s' não existe, criando-o\r\n", module_directory);
				
				if (!create_directory(module_directory)) {
					fprintf(stderr, "- Ocorreu um erro ao tentar criar o diretório!\r\n");
					exit(EXIT_FAILURE);
				}
			}
			
			for (size_t index = 0; index < module->pages.offset; index++) {
				struct Page* page = &module->pages.items[index];
				
				printf("+ Obtendo lista de páginas do módulo '%s'\r\n", module->name);
				
				if (get_page(&credentials, resource, page) != UERR_SUCCESS) {
					fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
					exit(EXIT_FAILURE);
				}
				
				printf("+ Verificando estado da página '%s'\r\n", page->name);
				
				char directory[strlen(page->name) + 1];
				strcpy(directory, page->name);
				normalize_filename(directory);
				
				char page_directory[strlen(module_directory) + strlen(PATH_SEPARATOR) + strlen(directory) + 1];
				strcpy(page_directory, module_directory);
				strcat(page_directory, PATH_SEPARATOR);
				strcat(page_directory, directory);
				
				if (!directory_exists(page_directory)) {
					fprintf(stderr, "- O diretório '%s' não existe, criando-o\r\n", page_directory);
					
					if (!create_directory(page_directory)) {
						fprintf(stderr, "- Ocorreu um erro ao tentar criar o diretório!\r\n");
						exit(EXIT_FAILURE);
					}
				}
				
				for (size_t index = 0; index < page->medias.offset; index++) {
					struct Media* media = &page->medias.items[index];
					
					char filename[strlen(page->name) + 1];
					strcpy(filename, page->name);
					normalize_filename(filename);
					
					char media_filename[strlen(page_directory) + strlen(PATH_SEPARATOR) + strlen(filename) + strlen(DOT) + strlen(MP4_FILE_EXTENSION) + 1];
					strcpy(media_filename, page_directory);
					strcat(media_filename, PATH_SEPARATOR);
					strcat(media_filename, filename);
					strcat(media_filename, DOT);
					strcat(media_filename, MP4_FILE_EXTENSION);
					
					if (!file_exists(media_filename)) {
						fprintf(stderr, "- O arquivo '%s' não existe, ele será baixado\r\n", media_filename);
						printf("+ Baixando de '%s' para '%s'\r\n", media->url, media_filename);
						
						struct String string = {0};
						
						curl_easy_setopt(curl, CURLOPT_URL, media->url);
						curl_easy_setopt(curl, CURLOPT_WRITEDATA, &string);
						
						if (curl_easy_perform(curl) != CURLE_OK) {
							return UERR_CURL_FAILURE;
						}
						
						struct Tags tags = {0};
						
						if (m3u8_parse(&tags, string.s) != UERR_SUCCESS) {
							fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
							exit(EXIT_FAILURE);
						}
						
						string_free(&string);
						
						int last_width = 0;
						const char* playlist_uri = NULL;
						
						for (size_t index = 0; index < tags.offset; index++) {
							struct Tag* tag = &tags.items[index];
							
							if (tag->type != EXT_X_STREAM_INF) {
								continue;
							}
							
							const struct Attribute* const attribute = attributes_get(&tag->attributes, "RESOLUTION");
							
							const char* const start = attribute->value;
							const char* const end = strstr(start, "x");
							
							const size_t size = (size_t) (end - start);
							
							char value[size + 1];
							memcpy(value, start, size);
							value[size] = '\0';
							
							const int width = atoi(value);
							
							if (last_width < width) {
								last_width = width;
								playlist_uri = tag->uri;
							}
						}
						
						CURLU *cu = curl_url();
						curl_url_set(cu, CURLUPART_URL, media->url, 0);
						curl_url_set(cu, CURLUPART_URL, playlist_uri, 0);
						
						m3u8_free(&tags);
						
						char* playlist_full_url = NULL;
						curl_url_get(cu, CURLUPART_URL, &playlist_full_url, 0);
						
						curl_easy_setopt(curl, CURLOPT_URL, playlist_full_url);
						
						//curl_free(url);
						
						if (curl_easy_perform(curl) != CURLE_OK) {
							return UERR_CURL_FAILURE;
						}
						
						if (m3u8_parse(&tags, string.s) != UERR_SUCCESS) {
							fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
							exit(EXIT_FAILURE);
						}
						
						int segment_number = 1;
						
						struct SegmentDownload downloads[tags.offset];
						size_t downloads_offset = 0;
						
						CURLM* multi_handle = curl_multi_init();
						
						if (multi_handle == NULL) {
							fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
							exit(EXIT_FAILURE);
						}
						
						curl_multi_setopt(multi_handle, CURLMOPT_MAX_HOST_CONNECTIONS, (long) 30);
						curl_multi_setopt(multi_handle, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long) 30);
						
						char playlist_filename[strlen(page_directory) + strlen(PATH_SEPARATOR) + strlen(LOCAL_PLAYLIST_FILENAME) + 1];
						strcpy(playlist_filename, page_directory);
						strcat(playlist_filename, PATH_SEPARATOR);
						strcat(playlist_filename, LOCAL_PLAYLIST_FILENAME);
						
						for (size_t index = 0; index < tags.offset; index++) {
							struct Tag* tag = &tags.items[index];
							
							if (tag->type == EXT_X_KEY) {
								struct Attribute* attribute = attributes_get(&tag->attributes, "URI");
								
								curl_url_set(cu, CURLUPART_URL, attribute->value, 0);
								
								char* url = NULL;
								curl_url_get(cu, CURLUPART_URL, &url, 0);
								
								char* filename = malloc(strlen(page_directory) + strlen(PATH_SEPARATOR) + strlen(KEY_FILE_EXTENSION) + strlen(DOT) + strlen(KEY_FILE_EXTENSION) + 1);
								strcpy(filename, page_directory);
								strcat(filename, PATH_SEPARATOR);
								strcat(filename, KEY_FILE_EXTENSION);
								strcat(filename, DOT);
								strcat(filename, KEY_FILE_EXTENSION);
								
								attribute_set_value(attribute, filename);
								tag_set_uri(tag, filename);
								
								printf("+ Baixando de '%s' para '%s'\r\n", url, filename);
								
								CURL* handle = curl_easy_init();
								
								if (handle == NULL) {
									fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
									exit(EXIT_FAILURE);
								}
								
								curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
								curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
								curl_easy_setopt(handle, CURLOPT_DOH_SSL_VERIFYPEER, 0L);
								curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
								curl_easy_setopt(handle, CURLOPT_USERAGENT, HTTP_DEFAULT_USER_AGENT);
								curl_easy_setopt(handle, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
								curl_easy_setopt(handle, CURLOPT_CAPATH, NULL);
								curl_easy_setopt(handle, CURLOPT_CAINFO, NULL);
								curl_easy_setopt(handle, CURLOPT_CAINFO_BLOB, &blob);
								curl_easy_setopt(handle, CURLOPT_RESOLVE, resolve_list);
								curl_easy_setopt(handle, CURLOPT_URL, url);
								
								curl_free(url);
								
								FILE* stream = fopen(filename, "wb");
								
								if (stream == NULL) {
									fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
									exit(EXIT_FAILURE);
								}
								
								curl_easy_setopt(handle, CURLOPT_WRITEDATA, (void*) stream);
								curl_multi_add_handle(multi_handle, handle);
								
								struct SegmentDownload download = {
									.handle = handle,
									.filename = filename,
									.stream = stream
								};
								
								downloads[downloads_offset++] = download;
								
								curl_url_set(cu, CURLUPART_URL, playlist_full_url, 0);
							} else if (tag->type == EXTINF && tag->uri != NULL) {
								curl_url_set(cu, CURLUPART_URL, tag->uri, 0);
								
								char* url = NULL;
								curl_url_get(cu, CURLUPART_URL, &url, 0);
								
								char value[intlen(segment_number) + 1];
								snprintf(value, sizeof(value), "%i", segment_number);
								
								char* filename = malloc(strlen(page_directory) + strlen(PATH_SEPARATOR) + strlen(value) + strlen(DOT) + strlen(TS_FILE_EXTENSION) + 1);
								strcpy(filename, page_directory);
								strcat(filename, PATH_SEPARATOR);
								strcat(filename, value);
								strcat(filename, DOT);
								strcat(filename, TS_FILE_EXTENSION);
								
								tag_set_uri(tag, filename);
								
								printf("+ Baixando de '%s' para '%s'\r\n", url, filename);
								
								CURL* handle = curl_easy_init();
								
								if (handle == NULL) {
									fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
									exit(EXIT_FAILURE);
								}
								
								curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
								curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
								curl_easy_setopt(handle, CURLOPT_DOH_SSL_VERIFYPEER, 0L);
								curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
								curl_easy_setopt(handle, CURLOPT_USERAGENT, HTTP_DEFAULT_USER_AGENT);
								curl_easy_setopt(handle, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
								curl_easy_setopt(handle, CURLOPT_CAPATH, NULL);
								curl_easy_setopt(handle, CURLOPT_CAINFO, NULL);
								curl_easy_setopt(handle, CURLOPT_CAINFO_BLOB, &blob);
								curl_easy_setopt(handle, CURLOPT_RESOLVE, resolve_list);
								curl_easy_setopt(handle, CURLOPT_URL, url);
								
								curl_free(url);
								
								FILE* stream = fopen(filename, "wb");
								
								if (stream == NULL) {
									fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
									exit(EXIT_FAILURE);
								}
								
								curl_easy_setopt(handle, CURLOPT_WRITEDATA, (void*) stream);
								curl_multi_add_handle(multi_handle, handle);
								
								struct SegmentDownload download = {
									.handle = handle,
									.filename = filename,
									.stream = stream
								};
								
								downloads[downloads_offset++] = download;
								
								segment_number++;
							}
							
						}
						
						curl_url_cleanup(cu);
						
						int still_running = 1;
						
						while (still_running) {
							CURLMcode mc = curl_multi_perform(multi_handle, &still_running);
							
							if (still_running) {
								mc = curl_multi_poll(multi_handle, NULL, 0, 1000, NULL);
							}
							
							if (mc) {
								break;
							}
						}
						
						CURLMsg* msg = NULL;
						int msgs_left = 0;
						
						while((msg = curl_multi_info_read(multi_handle, &msgs_left))) {
							if(msg->msg == CURLMSG_DONE) {
								
								/* Find out which handle this message is about */
								for (size_t index = 0; index < downloads_offset; index++) {
									struct SegmentDownload download = downloads[index];
									CURL* handle = download.handle;
									
									int found = (msg->easy_handle == handle);
									if(found)
										break;
								}
					
								printf("HTTP transfer completed with status %d\n", msg->data.result);
							}
						}
						
						printf("+ Exportando lista de reprodução para '%s'\r\n", playlist_filename);
						 
						FILE* const stream = fopen(playlist_filename, "wb");
						
						if (stream == NULL) {
							fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
							exit(EXIT_FAILURE);
						}
						
						tags_dumpf(&tags, stream);
						
						if (!tags_dumpf(&tags, stream)) {
							fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
							exit(EXIT_FAILURE);
						}
						
						fclose(stream);
						m3u8_free(&tags);
						
						for (size_t index = 0; index < downloads_offset; index++) {
							struct SegmentDownload* download = &downloads[index];
							
							fclose(download->stream);
							
							curl_multi_remove_handle(multi_handle, download->handle);
							curl_easy_cleanup(download->handle);
						}
						
						curl_multi_cleanup(multi_handle);
						
						char output_file[strlen(QUOTATION_MARK) * 2 + strlen(media_filename) + 1];
						strcpy(output_file, QUOTATION_MARK);
						strcat(output_file, media_filename);
						strcat(output_file, QUOTATION_MARK);
						
						printf("+ Copiando arquivos de mídia para '%s'\r\n", media_filename);
						
						const char* const command[][2] = {
							{"ffmpeg", NULL},
							{"-loglevel", "error"},
							{"-allowed_extensions", "ALL"},
							{"-i", playlist_filename},
							{"-c", "copy"},
							{"-movflags", "+faststart"},
							{"-map_metadata", "-1"},
							{output_file, NULL}
						};
						
						char command_line[5000];
						memset(command_line, '\0', sizeof(command_line));
						
						for (size_t index = 0; index < sizeof(command) / sizeof(*command); index++) {
							const char** const argument = (const char**) command[index];
							
							const char* const key = argument[0];
							const char* const value = argument[1];
							
							if (key != NULL) {
								strcat(command_line, key);
							}
							
							strcat(command_line, SPACE);
							
							if (value != NULL) {
								strcat(command_line, QUOTATION_MARK);
								strcat(command_line, value);
								strcat(command_line, QUOTATION_MARK);
							}
							
							strcat(command_line, SPACE);
						}
						
						const int exit_code = execute_shell_command(command_line);
						/*
						for (size_t index = 0; index < downloads_offset; index++) {
							struct SegmentDownload* download = &downloads[index];
							
							remove_file(download->filename);
							free(download->filename);
						}
						
						remove_file(playlist_filename);
						*/
						if (exit_code != 0) {
							fprintf(stderr, "- Ocorreu uma falha inesperada!\r\n");
							exit(EXIT_FAILURE);
						}
					}
				}
				
				for (size_t index = 0; index < page->attachments.offset; index++) {
					struct Attachment* attachment = &page->attachments.items[index];
					
					if (attachment->url != NULL) {
						printf("Attachment URL: %s\n", attachment->url);
					}
				}
			}
			
		}
	}
	
	return 0;
}

#if defined(WIN32) && defined(UNICODE)
	#define main wmain
#endif

int main() {
	printf("%i\n", b());
}
