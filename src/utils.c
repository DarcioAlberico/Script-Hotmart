#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "symbols.h"

#ifdef _WIN32
	#include <windows.h>
	#include <fileapi.h>
#else
	#include <unistd.h>
	#include <sys/stat.h>
	#include <errno.h>
	#include <glob.h>
#endif

static const char INVALID_FILENAME_CHARS[] = {
	'"', ' ', '/', '\\', ':', '*', '?', '\"', '<', '>', '|', '^', '\x00'
};

int execute_shell_command(const char* const command) {
	
	#if defined(_WIN32) && defined(UNICODE)
		const int wcsize = MultiByteToWideChar(CP_UTF8, 0, command, -1, NULL, 0);
		wchar_t wcommand[wcsize];
		MultiByteToWideChar(CP_UTF8, 0, command, -1, wcommand, wcsize);
		
		const int code = _wsystem(wcommand);
	#else
		const int code = system(command);
	#endif
	
	int exit_code = 0;
	
	#ifdef _WIN32
		exit_code = code;
	#else
		if (WIFSIGNALED(code)) {
			exit_code = 128 + WTERMSIG(code);
		} else {
			exit_code = WEXITSTATUS(code);
		}
	#endif
	
	return exit_code;
	
}

char* get_configuration_directory(void) {
	
	#ifdef _WIN32
		const char* const directory = getenv("APPDATA");
		
		if (directory == NULL) {
			return NULL;
		}
	#else
		const char* const directory = getenv("XDG_CONFIG_HOME");
		
		if (directory == NULL) {
			const char* const config = ".config";
			const char* const home = getenv("HOME");
			
			char* configuration_directory = malloc(strlen(home) + strlen(SLASH) + strlen(config) + strlen(SLASH) + 1);
			
			if (configuration_directory == NULL) {
				return NULL;
			}
			
			strcpy(configuration_directory, home);
			strcat(configuration_directory, SLASH);
			strcat(configuration_directory, config);
			strcat(configuration_directory, SLASH);
			
			return configuration_directory;
		}
	#endif
	
	const int trailing_separator = strlen(directory) > 0 && *(strchr(directory, '\0') - 1) == *PATH_SEPARATOR;
	char* configuration_directory = malloc(strlen(directory) + (trailing_separator ? 0 : strlen(PATH_SEPARATOR)) + 1);
	
	if (configuration_directory == NULL) {
		return NULL;
	}
	
	strcpy(configuration_directory, directory);
	
	if (!trailing_separator) {
		strcat(configuration_directory, PATH_SEPARATOR);
	}
	
	return configuration_directory;
	
}

void normalize_filename(char* filename) {
	
	char* ptr = strpbrk(filename, INVALID_FILENAME_CHARS);
	
	while (ptr != NULL) {
		*ptr = '_';
		ptr = strpbrk(ptr, INVALID_FILENAME_CHARS);
	}
	
}

char from_hex(const char ch) {
	
	if (ch <= '9' && ch >= '0') {
		return ch - '0';
	}
	
	 if (ch <= 'f' && ch >= 'a') {
		return ch - ('a' - 10);
	}
	
	if (ch <= 'F' && ch >= 'A') {
		return ch - ('A' - 10);
	}
	
	return '\0';
	
}

char to_hex(const char ch) {
	return ch + (ch > 9 ? ('a' - 10) : '0');
}

size_t intlen(const int value) {
	
	int val = value;
	size_t size = 0;
	
	do {
		val /= 10;
		size++;
	} while (val > 0);
	
	return size;
	
}

int isnumeric(const char* const s) {
	/*
	Return true (1) if the string is a numeric string, false (0) otherwise.
	
	A string is numeric if all characters in the string are numeric and there is at least one character in the string.
	*/
	
	for (size_t index = 0; index < strlen(s); index++) {
		const char ch = s[index];
		
		if (!isdigit(ch)) {
			return 0;
		}
	}
	
	return 1;
	
}

int expand_filename(const char* filename, char** fullpath) {
	
	#ifdef _WIN32
		#ifdef UNICODE
			const int wcsize = MultiByteToWideChar(CP_UTF8, 0, filename, -1, NULL, 0);
			wchar_t wfilename[wcsize];
			MultiByteToWideChar(CP_UTF8, 0, filename, -1, wfilename, wcsize);
			
			wchar_t path[MAX_PATH];
			const DWORD code = GetFullPathNameW(wfilename, sizeof(path) / sizeof(*path), path, NULL);
			
			if (code == 0) {
				return code;
			}
			
			wchar_t fpath[code];
			
			if (code > sizeof(path) / sizeof(*path)) {
				const DWORD code = GetFullPathNameW(wfilename, sizeof(fpath) / sizeof(*fpath), fpath, NULL);
				
				if (code == 0) {
					return code;
				}
			} else {
				wcscpy(fpath, path);
			}
			
			const int size = WideCharToMultiByte(CP_UTF8, 0, fpath, wcslen(fpath), NULL, 0, NULL, NULL);
			
			*fullpath = malloc(size);
			
			if (*fullpath == NULL) {
				return 0;
			}
			
			WideCharToMultiByte(CP_UTF8, 0, fpath, -1, *fullpath, size, NULL, NULL);
		#else
			char path[MAX_PATH];
			const DWORD code = GetFullPathNameA(filename, sizeof(path), path, NULL);
			
			if (code == 0) {
				return code;
			}
			
			*fullpath = malloc(code + 1);
			
			if (*fullpath == NULL) {
				return 0;
			}
			
			strcpy(*fullpath, path);
		#endif
	#else
		*fullpath = realpath(filename, NULL);
		
		if (*fullpath == NULL) {
			return 0;
		}
	#endif
	
	return 1;
	
}


int remove_file(const char* const filename) {
	/*
	Removes the file. On Windows, ignores the read-only attribute.
	*/
	
	#ifdef _WIN32
		#ifdef UNICODE
			const int wcsize = MultiByteToWideChar(CP_UTF8, 0, filename, -1, NULL, 0);
			wchar_t wfilename[wcsize];
			MultiByteToWideChar(CP_UTF8, 0, filename, -1, wfilename, wcsize);
			
			return DeleteFileW(wfilename) == 1;
		#else
			return DeleteFileA(filename) == 1;
		#endif
	#else
		return unlink(filename) == 0;
	#endif
	
}

int directory_exists(const char* const directory) {
	
	#ifdef _WIN32
		#ifdef UNICODE
			const int wcsize = MultiByteToWideChar(CP_UTF8, 0, directory, -1, NULL, 0);
			wchar_t wdirectory[wcsize];
			MultiByteToWideChar(CP_UTF8, 0, directory, -1, wdirectory, wcsize);
			
			const DWORD value = GetFileAttributesW(wdirectory);
		#else
			const DWORD value = GetFileAttributesA(directory);
		#endif
		
		return (value != -1 && ((value & FILE_ATTRIBUTE_DIRECTORY) > 0));
	#else
		struct stat st = {0};
		return (stat(directory, &st) >= 0 && S_ISDIR(st.st_mode));
	#endif
	
}

int file_exists(const char* const filename) {
	return 1;
	/*
	Returns 1 (true) if file exists and is a regular file or symlink, 0 (false) otherwise.
	Directories, device files, named pipes and sockets return false.
	*/
	
	#ifdef _WIN32
		#ifdef UNICODE
			const int wcsize = MultiByteToWideChar(CP_UTF8, 0, filename, -1, NULL, 0);
			wchar_t wfilename[wcsize];
			MultiByteToWideChar(CP_UTF8, 0, filename, -1, wfilename, wcsize);
			
			const DWORD value = GetFileAttributesW(wfilename);
		#else
			const DWORD value = GetFileAttributesA(filename);
		#endif
		
		return (value != -1 && ((value & FILE_ATTRIBUTE_DIRECTORY) == 0));
	#else
		struct stat st = {0};
		return (stat(filename, &st) == 0 && S_ISREG(st.st_mode));
	#endif
	
}

int is_absolute(const char* const path) {
	
	#ifdef _WIN32
		return (*path == *PATH_SEPARATOR || (strlen(path) > 1 && isalpha(*path) && path[1] == *COLON));
	#else
		return (*path == *PATH_SEPARATOR);
	#endif
	
}

static int raw_create_dir(const char* const directory) {
	
	#ifdef _WIN32
		#ifdef UNICODE
			const int wcsize = MultiByteToWideChar(CP_UTF8, 0, directory, -1, NULL, 0);
			wchar_t wdirectory[wcsize];
			MultiByteToWideChar(CP_UTF8, 0, directory, -1, wdirectory, wcsize);
			
			const BOOL code = CreateDirectoryW(wdirectory, NULL);
		#else
			const BOOL code = CreateDirectoryA(directory, NULL);
		#endif
			
		return (code == 1 || GetLastError() == ERROR_ALREADY_EXISTS);
	#else
		return (mkdir(directory, 0777) == 0 || errno == EEXIST);
	#endif
	
}

int create_directory(const char* const directory) {
	
	int omit_next = 0;
	
	#ifdef _WIN32
		omit_next = is_absolute(directory);
	#endif
	
	const char* start = directory;
	
	for (size_t index = 1; index < strlen(directory) + 1; index++) {
		const char* const ch = &directory[index];
		
		if (!(*ch == *PATH_SEPARATOR || (*ch == '\0' && index > 1 && *(ch - 1) != *PATH_SEPARATOR))) {
			continue;
		}
		
		if (omit_next) {
			omit_next = 0;
		} else {
			const size_t size = ch - start;
			
			char directory[size + 1];
			memcpy(directory, start, size);
			directory[size] = '\0';
			
			if (!raw_create_dir(directory)) {
				return 0;
			}
		}
	}
	
	return 1;
	
}

int iter(const char* const directory) {
	
	
	#ifdef _WIN32
		WIN32_FIND_DATA data = {};
		const HANDLE handle = FindFirstFileA(directory, &data);
		
		if (handle == INVALID_HANDLE_VALUE) {
			return 0;
		}
		
		while (1) {
			puts(data.cFileName);
			
			if (FindNextFileA(handle, &data) == 0) {
				FindClose(handle);
				
				if (GetLastError() == ERROR_NO_MORE_FILES) {
					break;
				}
				
				return 0;
			}
		}
		
		FindClose(handle);
		
	#else
		glob_t glb = {0};
		
		if (glob(directory, 0, NULL, &glb) != 0) {
			return 0;
		}
		
		for (size_t index = 0; index < glb.gl_pathc; index++) {
			const char* const filename = glb.gl_pathv[index];
			puts(filename);
		}
		
		globfree(&glb);
	#endif
	
	return 0;
}
/*
int main() {
	
	#ifdef _WIN32
		SetConsoleOutputCP(CP_UTF8);
		SetConsoleCP(CP_UTF8);
	#endif
	printf(("äAAàA"));
	printf("\r\n");
	printf("directory_exists -> %i\n", directory_exists("b"));
	create_directory("/home/snow/b/b/b");
	iter("./*");
}

int main(void) {
	char* a;
	expand_filename(".", &a);
	puts(a);
}
*/
