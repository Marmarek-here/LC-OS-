```
 _     ____  _____  ______  
| |   / ___|/ / _ \/ ___\ \ 
| |  | |   | | | | \___ \| |
| |__| |___| | |_| |___) | |
|_____\____| |\___/|____/| |
            \_\         /_/  
```


### LC(OS).
LC(OS) is a tiny OS that is made for fun, learning* and exploring**.   
*I made it with AI, while i focus on testing, maintaining and making ideas.   
**I may not fully understand all parts of the code yet, so feedback and pull requests are highly appreciated.   

### Dependencies required to build.
- GCC (i386-elf-gcc)
- GRUB
- G++
- Make
- libc-dev
- xorriso

### Dependencies required to run the OS.
- QEMU (x86_64, package name for Linux: qemu-system-x86_64)
- (Optional, but recommended) Hardware virtualization support.
- KVM/HAXM/WHPX (KVM for Linux, others for Windows)

### How to build.
- Make sure you have all the dependencies installed.
- Open a terminal/cmd.
- run 'git clone https://github.com/Marmarek-here/LC-OS-' and optionally add ' [folder name you want it to be in]'
- Navigate to the folder you have the source code in.
- Through GUI (a file explorer) or command prompt, create a folder called 'kernel'.
- Put everything in the source code folder into 'kernel'.
- Navigate to 'kernel' using a terminal (or through GUI, right-click empty space and "Open in terminal" or similar).
- Run 'make iso' inside the kernel folder

### How to run
- Run qemu-system-x86_64 -cdrom lcos.iso (or, if from the source code folder, kernel/lcos.iso) (add additonal flags to the command if you will to do so) in 'kernel' folder.
- Now it booted (or if it didn't, check if you skipped something).

-Note: i386-elf-gcc and i386-elf-binutils are required. Standard system GCC will likely fail to build the kernel correctly.

### Redistribution, Forks, Pull Requests permissions.
You are allowed to fork and send a pull request. You are allowed to download the project source code as MIT (license) says.   
However, you are not allowed what you are not allowed in the license.   
Pull requests are welcome as the project is being developed.   
You are allowed to redistribute the project with MIT's rules.   

### The system builds and runs successfully.
