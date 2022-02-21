# slab allocator

## 概述

之前通过`page allocator`进行管理所有内存，以`page`为单位，但是日常使用中经常需要小于`one page`的内存大小。如果需要分配`8Bytes`内存，但是分配了`one page`，过于浪费。于是，将多个`page`组成`one slab`，以`slab`为单位的`slab allocator`就诞生了，将`one slab`分成多个`object`，一个`object`可以等于任意大小字节。

这里是采用`slub`算法的`slab allocator`

## 原理

```
                       slab_cache_8bytes
                  ┌────►┌─────────────┐      ┌─────►┌────────┐
                  │     │ name        │      │      │ object │
                  │     ├─────────────┤      │  ┌──►├────────┤
                  │     │ align       │      │  │   │        ├──┐
                  │     ├─────────────┤      │  │   ├────────┤◄─┘
                  │     │ object_size │      │  │   │        ├──┐
                  │     ├─────────────┤      │  │   ├────────┤◄─┘
                  │     │ page        ├──────┘  │   │        ├──┐
                  │     ├─────────────┤         │   ├────────┤◄─┘
slab_cache        │     │ freelist    ├─────────┘   │        ├──►NULL
┌────────┐        │     ├─────────────┤             └────────┘
│ object ├────────┘     │ partial     ├────────────►┌────────┬───────►┌────────┐
├────────┤              ├─────────────┤             │ object │        │ object │
│ object ├────────┐     │ full        ├───────┐     ├────────┤        ├────────┤
├────────┤        │     └─────────────┘       │     │        ├──┐     │ object │
│        ├──┐     │                           │     ├────────┤◄─┘     ├────────┤
├────────┤◄─┘     │                           │     │        ├──┐     │ object │
│        ├──┐     │    slab_cache_16bytes     │     ├────────┤  │     ├────────┤
├────────┤◄─┘     └────►┌─────────────┐       │     │ object │  │     │        ├──►NULL
│        ├──►NULL       │ name        │       │     ├────────┤◄─┘     ├────────┤
└────────┘              ├─────────────┤       │     │        ├──►NULL │ object │
                        │ align       │       │     └────────┘        └────────┘
                        ├─────────────┤       └────►┌────────┐
                        │ object_size │             │ object │
                        ├─────────────┤             ├────────┤
                        │ page        │             │ object │
                        ├─────────────┤             ├────────┤
                        │ freelist    │             │ object │
                        ├─────────────┤             ├────────┤
                        │ partial     │             │ object │
                        ├─────────────┤             ├────────┤
                        │ full        │             │ object │
                        └─────────────┘             └────────┘
```

`slab allocator`整体布局，如上图所示，

每一个`slab cache`包含当前正在使用的`slab`（`page`成员指向此`slab`首地址，`freelist`成员指向此`slab`的第一个空闲`object`，同时第一个空闲`object`存储着下一个空闲`object`首地址，以此类推，最后一个空闲`object`指向`NULL`），其他`slab`存放在`partial`与`full`链表中。

* 如何从`slab cache`中分配`object`？

1. 检查当前正在使用的`slab`是否有空闲`object`？如果有，直接返回第一个空闲`object`；否则，
2. 检查当前正在使用的`slab` 是否 已满，如果是，将此`slab`移到`full`链表中；否则，
3. 检查`partial`链表中是否有空闲`object`的`slab`？ 如果有，将此`slab`移到 当前正在使用的`slab`，并且返回第一个空闲的`object`；否则，
4. 从`page allocator`中重新分配`new slab`

* 如何将`object`释放回`slab cahce`中？

1. 检查`object`是否属于当前正在使用的`slab`？如果是，直接释放`object`到当前正在使用的`slab`；否则，先将`object`释放回所属的`slab`中，然后
2. 检查`object`是否属于`full`链表中的`slab`？如果是，将此`slab`移到`partial`链表中；否则，代表`object`属于`partial`链表中的`slab`
3. 检查`object`对应的`partial`链表中的`slab` 是否 整体处于空闲状态？如果是，将此`slab`释放回`page allocator`；否则，什么都不用做，直接返回

## 如何使用？

使用`slab allocator`时，只需要在`page allocater`初始化结束后，调用`slab_cache_allocator_init()`即可，接下来就可以调用相关API进行 创建/销毁`slab cacha`，分配/释放内存

使用时记得加上头文件：`#include <memory/allocator/slabcache.h>`

## 功能

* 已完成功能：

1. 创建/销毁`slab cacha`
2. 从`slab cahce`中分配/释放指定大小的`object`

* 未完成功能：

1. per-cpu slab
2. 内存泄露、内存越界调试
