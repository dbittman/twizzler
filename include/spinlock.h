#pragma once
#include <stdatomic.h>
#include <guard.h>

struct spinlock {
	_Atomic int data;
};

#define SPINLOCK_INIT (struct spinlock) { .data = 0 }

void spinlock_acquire(struct spinlock *lock);
void spinlock_release(struct spinlock *lock);

#define spinlock_guard(s) guard2(s,spinlock_acquire,spinlock_release)

