#!/bin/sh
mkdir -p ../MyProject/wwwroot
em++ main.cpp -std=c++17 -sALLOW_MEMORY_GROWTH=1 -o ../MyProject/wwwroot/index.html -Os -Iraylib/include -Lraylib/lib -lraylib \
  -sUSE_GLFW=3 -sEXPORTED_RUNTIME_METHODS=stringToUTF8,UTF8ToString \
  --preload-file assets@assets --preload-file config.json@config.json --preload-file dialogue.json@dialogue.json \
  --shell-file shell.html -DPLATFORM_WEB