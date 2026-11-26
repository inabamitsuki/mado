# MADO - 窓

A small Windows utility for capturing Aero Glass windows

![Screenshot](1528.JPG)

## Build from sources

### Windows Vista & 7

``` gcc
gcc -m32 mado.c -o mado.exe -mwindows -ldwmapi -lgdi32 -luser32 -lcomdlg32 -static
```
