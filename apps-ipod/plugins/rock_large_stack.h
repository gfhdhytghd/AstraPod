/* Exclusive foreground launch on a larger stack. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_LARGE_STACK_H
#define ASTRAPOD_ROCK_LARGE_STACK_H

int rock_large_stack_launch(int (*launch)(void *), void *arg);

#endif
