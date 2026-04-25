//doomgeneric for cross-platform development library 'Simple DirectMedia Layer'

#include "doomkeys.h"
#include "i_video.h"
#include "m_argv.h"
#include "doomgeneric.h"

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#include <stdbool.h>
#include <ctype.h>

#include <badgevms/compositor.h>
#include <badgevms/framebuffer.h>
#include <badgevms/event.h>

window_handle_t window = NULL;
framebuffer_t *framebuffer;
struct timespec start_time;

#define KEYQUEUE_SIZE 16

static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

static unsigned char convertToDoomKey(keyboard_event_t event)
{
  char key;

  switch (event.scancode)
    {
    case KEY_SCANCODE_RETURN:
      key = KEY_ENTER;
      break;
    case KEY_SCANCODE_ESCAPE:
      key = KEY_ESCAPE;
      break;
    case KEY_SCANCODE_LEFT:
      key = KEY_LEFTARROW;
      break;
    case KEY_SCANCODE_RIGHT:
      key = KEY_RIGHTARROW;
      break;
    case KEY_SCANCODE_UP:
      key = KEY_UPARROW;
      break;
    case KEY_SCANCODE_DOWN:
      key = KEY_DOWNARROW;
      break;
    case KEY_SCANCODE_LCTRL:
    case KEY_SCANCODE_RCTRL:
      key = KEY_FIRE;
      break;
    case KEY_SCANCODE_SPACE:
      key = KEY_USE;
      break;
    case KEY_SCANCODE_LSHIFT:
    case KEY_SCANCODE_RSHIFT:
      key = KEY_RSHIFT;
      break;
    case KEY_SCANCODE_LALT:
    case KEY_SCANCODE_RALT:
      key = KEY_LALT;
      break;
    case KEY_SCANCODE_F2:
      key = KEY_F2;
      break;
    case KEY_SCANCODE_F3:
      key = KEY_F3;
      break;
    case KEY_SCANCODE_F4:
      key = KEY_F4;
      break;
    case KEY_SCANCODE_F5:
      key = KEY_F5;
      break;
    case KEY_SCANCODE_F6:
      key = KEY_F6;
      break;
    case KEY_SCANCODE_F7:
      key = KEY_F7;
      break;
    case KEY_SCANCODE_F8:
      key = KEY_F8;
      break;
    case KEY_SCANCODE_F9:
      key = KEY_F9;
      break;
    case KEY_SCANCODE_F10:
      key = KEY_F10;
      break;
    case KEY_SCANCODE_F11:
      key = KEY_F11;
      break;
    case KEY_SCANCODE_EQUALS:
      key = KEY_EQUALS;
      break;
    case KEY_SCANCODE_MINUS:
      key = KEY_MINUS;
      break;
    default:
      if (event.text) {
        key = tolower(event.text);
      }
      break;
    }

  return key;
}

static void addKeyToQueue(int pressed, keyboard_event_t event)
{
  unsigned char key = convertToDoomKey(event);

  unsigned short keyData = (pressed << 8) | key;

  s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
  s_KeyQueueWriteIndex++;
  s_KeyQueueWriteIndex %= KEYQUEUE_SIZE;
}

static void handleKeyInput()
{
  event_t e;
  do {
    e = window_event_poll(window, false, 0);
    if (e.type == EVENT_QUIT){
      puts("Quit requested");
      exit(1);
    }
    if (e.type == EVENT_KEY_DOWN) {
      if (e.keyboard.scancode == KEY_SCANCODE_RETURN && e.keyboard.mod & BADGEVMS_KMOD_LALT) {
        window_flag_t flags = window_flags_get(window);
        window_flags_set(window, flags ^ WINDOW_FLAG_FULLSCREEN);
      } else {
        addKeyToQueue(1, e.keyboard);
      }
    } else if (e.type == EVENT_KEY_UP) {
      if (! (e.keyboard.scancode == KEY_SCANCODE_RETURN && e.keyboard.mod & BADGEVMS_KMOD_LALT)) {
        addKeyToQueue(0, e.keyboard);
      }
    }
  } while (e.type != EVENT_NONE);
}

void DG_Init()
{
  window = window_create("DOOM", (window_size_t){DOOMGENERIC_RESX, DOOMGENERIC_RESY}, WINDOW_FLAG_DOUBLE_BUFFERED);
  // Let BadgeVMS do hardware scaling for us
  framebuffer = window_framebuffer_create(window, (window_size_t){SCREENWIDTH, SCREENHEIGHT}, BADGEVMS_PIXELFORMAT_BGR565);

  DG_ScreenBuffer = (void*)framebuffer->pixels;
  clock_gettime(CLOCK_MONOTONIC, &start_time);
}

void DG_DrawFrame()
{
    window_present(window, false, NULL, 0);
    handleKeyInput();
}

void DG_SleepMs(uint32_t ms)
{
  usleep(ms);
}

uint32_t DG_GetTicksMs()
{
  struct timespec cur_time;
  clock_gettime(CLOCK_MONOTONIC, &cur_time);
  long elapsed_us = (cur_time.tv_sec - start_time.tv_sec) * 1000000L + (cur_time.tv_nsec - start_time.tv_nsec) / 1000L;

  return elapsed_us / 1000;
}

int DG_GetKey(int* pressed, unsigned char* doomKey)
{
  if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex){
    //key queue is empty
    return 0;
  }else{
    unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
    s_KeyQueueReadIndex++;
    s_KeyQueueReadIndex %= KEYQUEUE_SIZE;

    *pressed = keyData >> 8;
    *doomKey = keyData & 0xFF;

    return 1;
  }

  return 0;
}

void DG_SetWindowTitle(const char * title)
{
  window_title_set(window, title);
}

int main(int argc, char **argv)
{
  doomgeneric_Create(argc, argv);

  for (int i = 0; ; i++)
  {
      doomgeneric_Tick();
  }
  

  return 0;
}
