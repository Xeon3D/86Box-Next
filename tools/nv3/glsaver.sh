#!/bin/bash
# glsaver.sh NAME: run Win98's OpenGL screensaver "3D NAME" (Pipes, Maze, Text, Flower Box,
# Flying Objects) full screen: "c:\windows\system\3d NAME.scr" /s. PT keyboard: '"' Shift+2,
# ':' Shift+period, '\' the key left of 1, '/' Shift+7.
T=$(dirname "$0")/nvtest.sh
Q="key 2a,03,83,aa"; C="key 2a,34,b4,aa"; B="key 29,a9"; S="key 2a,08,88,aa"
$T cmd "key 1d,01,81,9d"; sleep 1; $T cmd "type r"; sleep 2
$T cmd "$Q" "type c" "$C" "$B" "type windows" "$B" "type system" "$B" "type 3d $1.scr" "$Q" "type  " "$S" "type s" "key 1c,9c"
