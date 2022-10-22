int directory_exists(const char* const directory);
int file_exists(const char* const filename);
int create_directory(const char* const directory);
char to_hex(const char ch);
char from_hex(const char ch);
int isnumeric(const char* const s);
void normalize_filename(char* filename);
int expand_filename(const char* filename, char** fullpath);
int execute_shell_command(const char* const command);