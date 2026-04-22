/* Auto-generated file contents and entries - do not edit */

static const char file_content_configlcos_hostname_conf[] = "lcos\n";
static const char file_content_configlcos_login_banner_txt[] = "LC(OS) \\n \\l\n";
static const char file_content_configlcos_motd_txt[] = "Welcome to LC(OS).\nThis system now ships with a Linux-like directory layout.\n";
static const char file_content_configlcos_mounts_conf[] = "# device          mountpoint  type   options\n/devices/lcos-disk0   /           fat    rw\n";
static const char file_content_configlcos_release_conf[] = "NAME=LCOS\nVERSION=0.1\nID=lcos\nPRETTY_NAME=LC(OS) 0.1\nHOME_URL=https://local.lcos\n";
static const char file_content_configlcos_shells_list[] = "/cmd/lsh\n/cmd/lcbat\n";
static const char file_content_configlcos_users_db[] = "root:x:0:0:root:/home:/cmd\nguest:x:1000:1000:guest:/home:/cmd\n";
static const char file_content_deviceslcos_console_dev[] = "Primary system console device placeholder.\n";
static const char file_content_deviceslcos_null_dev[] = "Character device placeholder for null sink.\n";
static const char file_content_docsmanual_md[] = "# LC(OS) Manual\n\n## Reading Files\n- Use `read /docs/manual.md` to view Markdown with terminal formatting.\n- Use `read /docs/manual.txt` for the plain-text variant.\n\n## Batch Scripts\n- `run /programs/demo.lcbat` runs the native batch example.\n- `run /programs/demo.bat` runs the DOS-style batch example.\n\n## Built-in Commands\n- `ls` lists files and directories.\n- `read` prints file contents.\n- `date` shows the RTC date and time.\n";
static const char file_content_docsmanual_txt[] = "LC(OS) User Manual\nThis is a simple 32-bit kernel with VGA terminal support.\n";
static const char file_content_extrassharereadme_txt[] = "This directory contains shared documentation and system text files.\n";
static const char file_content_homeuser_txt[] = "User profile data for LC(OS)\n";
static const char file_content_lcos_init_lcbat[] = "echo LC(OS) startup sequence\necho Type help to list commands\n";
static const char file_content_lcos_version_txt[] = "LC(OS) 0.1\nKernel: lcos.elf\nFilesystem profile: FAT12/16 + persistent overlay\n";
static const char file_content_programsdemo_bat[] = "@echo off\nREM LC(OS) DOS-style batch example\n\ndate\nver\ndir /programs\ntype /docs/manual.txt\n";
static const char file_content_programsdemo_lcbat[] = "# LC(OS) batch script example\n# Lines starting with # or ; are comments\n\necho Running demo script...\nls\nmkdir demo_dir\ntouch demo_dir/note.txt\necho Hello from LC(OS) > demo_dir/note.txt\nread demo_dir/note.txt\nrun snake\n";
static const char file_content_programsdesktop[] = "LC(OS) desktop launcher entry.\nRun with: run desktop\n";
static const char file_content_programshello_asm[] = "; Tiny LC(OS) ASM-style source program\nprint \"Hello from /programs/hello.asm\"\nmsg \"This is running through the tiny ASM runner.\"\nnewline\nputs \"Done.\"\nexit\n";
static const char file_content_programshello_c[] = "int main(void)\n{\n    puts(\"Hello from /programs/hello.c\");\n    puts(\"LC(OS) tiny C runner executed this source file.\");\n    return 0;\n}\n";
static const char file_content_programspingpong[] = "LC(OS) Ping Pong game program\nRun with: run pingpong\nUse Up/Down against CPU.\nPress Ctrl+C to exit.\n";
static const char file_content_programssnake[] = "LC(OS) Snake game program\nRun with: run snake\nUse arrow keys to steer.\nPress Ctrl+C to exit.\n";
static const char file_content_programstetris[] = "LC(OS) Tetris game program\nRun with: run tetris\nUse Left/Right/Up/Down.\nPress Ctrl+C to exit.\n";
static const char file_content_readme_txt[] = "This folder is the build-time filesystem source for LC(OS).\n\nLayout now uses LC(OS)-style top-level dirs such as:\n/\n/addons\n/cmd\n/config\n/devices\n/home\n/mounts\n/programs\n/runtime\n/scratch\n/startup\n/state\n/status\n/syscmd\n\nSystem metadata and config samples are included in /config and /status.\n";
static const char file_content_scratch_keep[] = "Temporary directory marker.\n";
static const char file_content_startuplcos_boot_conf[] = "default=lcos\nkernel=/startup/lcos.elf\n";
static const char file_content_statelogboot_log[] = "[boot] LC(OS) filesystem initialized\n[boot] VFS overlay ready\n";
static const char file_content_statuslcos_cpu_status[] = "processor	: 0\nvendor_id	: Generic x86\nmodel name	: LCOS virtual CPU\n";
static const char file_content_statuslcos_memory_status[] = "MemTotal: 131072 kB\nMemFree:  65536 kB\n";
static const char file_content_welcome_txt[] = "Welcome to LC(OS)\nIt's a basic OS!!!\nIt gives off MS-DOS vibe!\nITs live!\nAnd i love it!\n";

static const struct FileEntry fs_files[] = {
    {"lcos-hostname.conf", "/config/lcos-hostname.conf", "/config", file_content_configlcos_hostname_conf},
    {"lcos-login-banner.txt", "/config/lcos-login-banner.txt", "/config", file_content_configlcos_login_banner_txt},
    {"lcos-motd.txt", "/config/lcos-motd.txt", "/config", file_content_configlcos_motd_txt},
    {"lcos-mounts.conf", "/config/lcos-mounts.conf", "/config", file_content_configlcos_mounts_conf},
    {"lcos-release.conf", "/config/lcos-release.conf", "/config", file_content_configlcos_release_conf},
    {"lcos-shells.list", "/config/lcos-shells.list", "/config", file_content_configlcos_shells_list},
    {"lcos-users.db", "/config/lcos-users.db", "/config", file_content_configlcos_users_db},
    {"lcos-console.dev", "/devices/lcos-console.dev", "/devices", file_content_deviceslcos_console_dev},
    {"lcos-null.dev", "/devices/lcos-null.dev", "/devices", file_content_deviceslcos_null_dev},
    {"manual.md", "/docs/manual.md", "/docs", file_content_docsmanual_md},
    {"manual.txt", "/docs/manual.txt", "/docs", file_content_docsmanual_txt},
    {"readme.txt", "/extras/share/readme.txt", "/extras/share", file_content_extrassharereadme_txt},
    {"user.txt", "/home/user.txt", "/home", file_content_homeuser_txt},
    {"lcos-init.lcbat", "/lcos-init.lcbat", "/", file_content_lcos_init_lcbat},
    {"lcos-version.txt", "/lcos-version.txt", "/", file_content_lcos_version_txt},
    {"demo.bat", "/programs/demo.bat", "/programs", file_content_programsdemo_bat},
    {"demo.lcbat", "/programs/demo.lcbat", "/programs", file_content_programsdemo_lcbat},
    {"desktop", "/programs/desktop", "/programs", file_content_programsdesktop},
    {"hello.asm", "/programs/hello.asm", "/programs", file_content_programshello_asm},
    {"hello.c", "/programs/hello.c", "/programs", file_content_programshello_c},
    {"pingpong", "/programs/pingpong", "/programs", file_content_programspingpong},
    {"snake", "/programs/snake", "/programs", file_content_programssnake},
    {"tetris", "/programs/tetris", "/programs", file_content_programstetris},
    {"readme.txt", "/readme.txt", "/", file_content_readme_txt},
    {".keep", "/scratch/.keep", "/scratch", file_content_scratch_keep},
    {"lcos-boot.conf", "/startup/lcos-boot.conf", "/startup", file_content_startuplcos_boot_conf},
    {"boot.log", "/state/log/boot.log", "/state/log", file_content_statelogboot_log},
    {"lcos-cpu.status", "/status/lcos-cpu.status", "/status", file_content_statuslcos_cpu_status},
    {"lcos-memory.status", "/status/lcos-memory.status", "/status", file_content_statuslcos_memory_status},
    {"welcome.txt", "/welcome.txt", "/", file_content_welcome_txt},
    {0, 0, 0, 0}
};
