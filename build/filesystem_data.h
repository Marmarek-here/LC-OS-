/* Auto-generated file contents and entries - do not edit */

static const char file_content_docsmanual_md[] = "# LC(OS) Manual\n\n## Reading Files\n- Use `read /docs/manual.md` to view Markdown with terminal formatting.\n- Use `read /docs/manual.txt` for the plain-text variant.\n\n## Batch Scripts\n- `run /programs/demo.lcbat` runs the native batch example.\n- `run /programs/demo.bat` runs the DOS-style batch example.\n\n## Built-in Commands\n- `ls` lists files and directories.\n- `read` prints file contents.\n- `date` shows the RTC date and time.\n";
static const char file_content_docsmanual_txt[] = "LC(OS) User Manual\nThis is a simple 32-bit kernel with VGA terminal support.\n";
static const char file_content_homeuser_txt[] = "User profile data for LC(OS)\n";
static const char file_content_programsdemo_bat[] = "@echo off\nREM LC(OS) DOS-style batch example\n\ndate\nver\ndir /programs\ntype /docs/manual.txt\n";
static const char file_content_programsdemo_lcbat[] = "# LC(OS) batch script example\n# Lines starting with # or ; are comments\n\necho Running demo script...\nls\nmkdir demo_dir\ntouch demo_dir/note.txt\necho Hello from LC(OS) > demo_dir/note.txt\nread demo_dir/note.txt\nrun snake\n";
static const char file_content_programshello_asm[] = "; Tiny LC(OS) ASM-style source program\nprint \"Hello from /programs/hello.asm\"\nmsg \"This is running through the tiny ASM runner.\"\nnewline\nputs \"Done.\"\nexit\n";
static const char file_content_programshello_c[] = "int main(void)\n{\n    puts(\"Hello from /programs/hello.c\");\n    puts(\"LC(OS) tiny C runner executed this source file.\");\n    return 0;\n}\n";
static const char file_content_programspingpong[] = "LC(OS) Ping Pong game program\nRun with: run pingpong\nUse Up/Down against CPU.\nPress Ctrl+C to exit.\n";
static const char file_content_programssnake[] = "LC(OS) Snake game program\nRun with: run snake\nUse arrow keys to steer.\nPress Ctrl+C to exit.\n";
static const char file_content_programstetris[] = "LC(OS) Tetris game program\nRun with: run tetris\nUse Left/Right/Up/Down.\nPress Ctrl+C to exit.\n";
static const char file_content_readme_txt[] = "This folder is the tiny build-time filesystem source.\n";
static const char file_content_welcome_txt[] = "Welcome to LC(OS)\nIt's a basic OS!!!\nIt gives off MS-DOS vibe!\nITs live!\nAnd i love it!\n";

static const struct FileEntry fs_files[] = {
    {"manual.md", "/docs/manual.md", "/docs", file_content_docsmanual_md},
    {"manual.txt", "/docs/manual.txt", "/docs", file_content_docsmanual_txt},
    {"user.txt", "/home/user.txt", "/home", file_content_homeuser_txt},
    {"demo.bat", "/programs/demo.bat", "/programs", file_content_programsdemo_bat},
    {"demo.lcbat", "/programs/demo.lcbat", "/programs", file_content_programsdemo_lcbat},
    {"hello.asm", "/programs/hello.asm", "/programs", file_content_programshello_asm},
    {"hello.c", "/programs/hello.c", "/programs", file_content_programshello_c},
    {"pingpong", "/programs/pingpong", "/programs", file_content_programspingpong},
    {"snake", "/programs/snake", "/programs", file_content_programssnake},
    {"tetris", "/programs/tetris", "/programs", file_content_programstetris},
    {"readme.txt", "/readme.txt", "/", file_content_readme_txt},
    {"welcome.txt", "/welcome.txt", "/", file_content_welcome_txt},
    {0, 0, 0, 0}
};
