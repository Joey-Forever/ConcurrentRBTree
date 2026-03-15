1. This implements a sorted associative container that supports only unique keys.  (Similar to std::set and folly::ConcurrentSkipList.)
2. 该容器直接对标folly::ConcurrentSkipList（GC回收策略、iterator、Accessor访问接口与其一致），支持超大规模数据量（百万级 ～ 千万级）下的高并发“多读少写”场景（尤其是写概率低于20%时）。在该场景下，gipsy_danger::ConcurrentRBTree
拥有比folly::ConcurrentSkipList最大1.7x的并发读性能和并发写性能（comparision_test测试下的数据）。
以下是在13th Gen Intel(R) Core(TM) i9-13900K机器下（16核32线程），测试得出的分别在16线程和27线程（不选32线程是为了避免其他用户进程和系统进程干扰，事实上27线程下测试双方的读写吞吐都达到了峰值）下folly::ConcurrentSkipList和gipsy_danger::ConcurrebtRBTree各自并发读写的性能数据。其中图表的横坐标是写概率、纵坐标是读吞吐量/写吞吐量，初始的数据量都是8000000，所有线程均可进行并发读写。
             (这里需要贴上x86_result子目录下的throughput_16threads_8000000init.jpg和throughput_27threads_8000000init.jpg这两张图)
可以看到，即使在高写概率下gipsy_danger::ConcurrentRBTree的性能下降明显，但是整体平均值上还是保持了1.4x ～ 1.5x的性能优势，值得注意的是，如果看低写概率如10%（通用内存缓存的典型写概率，正是gipsy_danger::ConcurrentRBTree的主打场景）下的表现的话，gipsy_danger::ConcurrentRBTree的优势就更明显了，16线程下～1.6x的读优势和～1.5x的写优势，27线程下～1.7x的读优势和～1.6x写优势。
gipsy_danger::ConcurrentRBTree领先原因：
                           （这里应该贴上x86_result子目录下的cpu_cache_usage.jpg这张图）
如上图所示，在相同的任务下，尽管folly::ConcurrentSkipList在cpu缓存命中率上比gipsy_danger::ConcurrentRBTree要稍优（主要是由于gipsy_danger::ConcurrentRBTree的node size相对更大，需要占据更大的存储空间），但是由于folly::ConcurrentSkipList的数据访问总次数和cpu指令执行总次数比gipsy_danger::ConcurrentRBTree多了将近1倍，这直接导致了在整体性能上folly::ConcurrentSkipList仍然比gipsy_danger::ConcurrentRBTree要差的多。

如想要了解更多更仔细的测试数据（如不同线程数、不同初始数据量），可以进入这里查看（这里应该贴上当前仓库的x86_result这个子目录的链接）。

Note: 虽然测试时的读操作都是单点读，但是由于设计上，gipsy_danger::ConcurrentRBTree内部额外维护了一个有序链表，所以范围查找的性能比传统红黑树的中序遍历要高效的多，性能上和链表是一致的。
3. trade off:
正如“天下没有免费的午餐”这句谚语所言，gipsy_danger::ConcurrentRBTree的优秀表现实际上也是一场trade off。
3.1. 高写概率下的性能牺牲：为了最大化低写概率下的整体性能以及简化设计，gipsy_danger::ConcurrentRBTree并没有实现真正意义上的低粒度并发写（众所周知，由于红黑树的树结构以及旋转等平衡操作，这需要极其精细的并发控制策略）,并发的树结构修改操作仍然是串行的。在低写概率下，这种开销是极小的，但当写概率逐步上升时，写冲突会越来越剧烈，这直接导致了高写概率下其整体性能有时甚至比folly::ConcurrentSkipList要弱上不少。不过，这种trade off是值得的，这极大的简化了结构的设计，并且为低写概率场景带来了相当可观的性能提升。
3.2. more memory usage (larger node): gipsy_danger::ConcurrentRBTree is about 30% (1M datas, 4 bytes each value) more than folly::ConcurrentSkipList. but as the value size increases, the disadvantage of node size would decreases. 这直接导致了gipsy_danger::ConcurrentRBTree的cpu缓存命中率比folly::ConcurrentSkipList要稍差，不过这似乎并不影响其高性能。
4. usage：
gipsy_danger::ConcurrentRBTree是一个header only的实现，这意味着你只需在编译你的程序时在编译命令中包含本库的include目录，然后就可以在代码中#include <ConcurrentRBTree.h>并使用gipsy_danger::ConcurrentRBTree了。需要注意的是，如果你不需要启用debug功能（这将在代码中嵌入大量的assert），请在编译时额外加上-DNDEBUG参数。

Note：gipsy_danger::ConcurrentRBTree在使用上和folly::ConcurrentSkipList完全一致，除了暂不支持const_iterator以及自定义比较器的模版参数（不过你可以对value type进行自定义封装类来实现）之外，其他接口完全一致。

