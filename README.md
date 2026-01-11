This is an attempt to reverse engineer USB webcams based on the Sonix chipsets. I tested it on SN9C286, but it might work on other similar chipsets. Since this was a small hobby project, I 'vibecoded' it with Gemini, but I fixed some of the mistakes by hand. It runs on console, and needs Linux, although it might run on other similar OSes as well.
I wanted to have access to some webcam settings that are not exposed by standard UVC controls. I was interested in setting the real (analogue) gain and longer exposure times. I wanted to disable NR (noise reduction) as well, but I eventually realized that the chipset does not apply any NR at lower sensitivity levels.
My board was very cheap, under 20 USD from AliExpress. The Sonix chip is paired with a Sony IMX298.

To compile it, just use: `gcc -o xu_analyze xu_analyze.c -lpthread`
I run it as root, but it might work without root as well.
Here is how to use it: Start a the webcam with a live stream in some external app such as guvcview. In a terminal window, start xu_analyze. For better results, set that window as Allways on top. Change the webcam settings and see what registers change. Make sure to explore the entire memory space range, which is 8kb. You can also modify random registers and see if the picture changes in any way.
Some registers will crash the camera, so you need to unplug it and plug it back. Others will take effect only after you restart the video stream. Some are overriden at a videostream restart, others are not. Some register changes will take effect imediately.
You can modify each register bit by bit, or you can fuzz a whole line at once (the line with the cursor).

Shortcuts:

### ⌨️ Keyboard Controls
| Key | Action |
| :--- | :--- |
| `r` / `R` | **Line Fuzz**: Randomize all 16 bytes on the current cursor row. |
| `Ctrl+G` | **Go To**: Open hex address jump dialog. |
| `PgUp` / `PgDn`| **Page Scroll**: Jump the view by 256 bytes. |
| `q` | **Quit**: Restore terminal and exit. |
| `Enter` / `Esc`| **Confirm/Cancel**: Used within GOTO or HEX EDIT modes. |

### 🖱 Mouse Interaction
* **Grid Selection**: Click any byte to select it and see its bit-breakdown.
* **Scroll Wheel**: Navigate up or down by 16-byte increments.
* **Scroll Bar**: Click the vertical track on the right to jump through the 8KB space.
* **Bit Toggle**: Click individual bit numbers (0-7) in the edit row to flip bits.

### 🕹 Action Buttons (Bottom Row)
* **[DUMP]**: Dumps the current 8192-byte RAM state to `dump.bin`.
* **[GOTO]**: Opens the address jump prompt.
* **[FUZZ-LN]**: Randomizes the registers on the current cursor line.
* **[SET]**: Commits your staged hex/bit changes to the hardware.
