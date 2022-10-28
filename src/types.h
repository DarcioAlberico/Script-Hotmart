#include <stdlib.h>

struct Credentials {
	char* access_token;
	char* refresh_token;
	int expires_in;
};

struct Media {
	char* url;
};

struct Medias {
	size_t offset;
	size_t size;
	struct Media* items;
};

struct Attachment {
	char* url;
	char* extension;
};

struct Attachments {
	size_t offset;
	size_t size;
	struct Attachment* items;
};

struct Page {
	char* id;
	char* name;
	struct Medias medias;
	struct Attachments attachments;
};

struct Pages {
	size_t offset;
	size_t size;
	struct Page* items;
};

struct Module {
	char* id;
	char* name;
	char* download_location;
	struct Pages pages;
};

struct Modules {
	size_t offset;
	size_t size;
	struct Module* items;
};

struct Resource {
	char* name;
	char* subdomain;
	char* download_location;
	struct Modules modules;
};

struct Resources {
	size_t offset;
	size_t size;
	struct Resource* items;
};

struct String {
	char *s;
	size_t slength;
};

void string_free(struct String* obj);

#pragma once