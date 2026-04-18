/* Auto-generated file contents and entries - do not edit */

static const char file_content_docsmanual_txt[] = "LC(OS) User Manual\nThis is a simple 32-bit kernel with VGA terminal support.\n";
static const char file_content_homeuser_txt[] = "User profile data for LC(OS)\n";
static const char file_content_programshello_asm[] = "; Tiny LC(OS) ASM-style source program\nprint \"Hello from /programs/hello.asm\"\nmsg \"This is running through the tiny ASM runner.\"\nnewline\nputs \"Done.\"\nexit\n";
static const char file_content_programshello_c[] = "int main(void)\n{\n    puts(\"Hello from /programs/hello.c\");\n    puts(\"LC(OS) tiny C runner executed this source file.\");\n    puts(\"I like this OS lolololol\");\n    return 0;\n}\n";
static const char file_content_programspingpong[] = "LC(OS) Ping Pong game program\nRun with: run pingpong\nUse Up/Down against CPU.\nPress Ctrl+C to exit.\n";
static const char file_content_programssnake[] = "LC(OS) Snake game program\nRun with: run snake\nUse arrow keys to steer.\nPress Ctrl+C to exit.\n";
static const char file_content_programstetris[] = "LC(OS) Tetris game program\nRun with: run tetris\nUse Left/Right/Up/Down.\nPress Ctrl+C to exit.\n";
static const char file_content_readme_txt[] = "This folder is the tiny build-time filesystem source.\n";
static const char file_content_welcome_txt[] = "Welcome to LC(OS)\nIt's a basic OS!!!\nIt gives off MS-DOS vibe!\nITs live!\nAnd i love it!\n";

static const struct FileEntry fs_files[] = {
    {"manual.txt", "/docs/manual.txt", "/docs", file_content_docsmanual_txt},
    {"user.txt", "/home/user.txt", "/home", file_content_homeuser_txt},
    {"hello.asm", "/programs/hello.asm", "/programs", file_content_programshello_asm},
    {"hello.c", "/programs/hello.c", "/programs", file_content_programshello_c},
    {"pingpong", "/programs/pingpong", "/programs", file_content_programspingpong},
    {"snake", "/programs/snake", "/programs", file_content_programssnake},
    {"tetris", "/programs/tetris", "/programs", file_content_programstetris},
    {"readme.txt", "/readme.txt", "/", file_content_readme_txt},
    {"welcome.txt", "/welcome.txt", "/", file_content_welcome_txt},
    {0, 0, 0, 0}
};
