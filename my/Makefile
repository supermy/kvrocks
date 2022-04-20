clean:
	rm -rf target/*
	rm -rf logs/*

# -E: 对目标文件只进行预处理，生成的还是源码。
# -S: 对目标文件进行预处理、编译操作，生成汇编代码。
# -o: 对目标文件进行预处理、编译和汇编操作，生效obj文件。
# -fsyntax-only : 对代码只进行语法检查，不做其它任何动作。
# 地址无关的代码 -fPIC
# 地址无关可执行 -fPIE
# -c（常用）
# 编译或汇编源文件，但是不作连接，编译器输出对应于源文件的目标文件，即.o文件。


# 学习 c++  -bundle -undefined dynamic_lookup -I../external/glog/src -I../external/glog/src 
target/test/cc/%: test/cc/%.cc
	mkdir -p target/test/cc
	# g++ -E -C $<
	# g++ -MMD $<
	# -g++ -fsyntax-only -Wall -std=c++11 $<  -I/usr/local/include/
	scan-build -o target/test/cgdb-scan g++ -std=c++11 $< -o $@ -lpthread ${COMPCFG}


# make test/c/c_test
# .PHONY: 
src/cc/%:target/src/cc/%
	$<
test/cc/%:target/test/cc/%
	$< ${RUNCFG}

# GLOG支持以下功能：brew install glog
#    ◆ 参数设置，以命令行参数的方式设置标志参数来控制日志记录行为；
#    ◆ 严重性分级，根据日志严重性分级记录日志；
#    ◆ 可有条件地记录日志信息；
#    ◆ 条件中止程序。丰富的条件判定宏，可预设程序终止条件；
#    ◆ 异常信号处理。程序异常情况，可自定义异常处理过程；
#    ◆ 支持debug功能；
#    ◆ 自定义日志信息；
#    ◆ 线程安全日志记录方式；
#    ◆ 系统级日志记录；
#    ◆ google perror风格日志信息；
#    ◆ 精简日志字符串信息

glog:
	-make RUNCFG=" 4 logs" COMPCFG="-lglog " test/cc/glog
events:
	-make RUNCFG=" " COMPCFG="-levent -I/usr/local/include/" test/cc/event_server
eventc:
	-make RUNCFG=" 127.0.0.1 9999 " COMPCFG="-levent -I/usr/local/include/" test/cc/event_client 
eventsbuf:
	-make RUNCFG=" " COMPCFG="-levent -I/usr/local/include/" test/cc/event_buf_server
eventcbuf:
	-make RUNCFG=" 127.0.0.1 9999 " COMPCFG="-levent -I/usr/local/include/" test/cc/event_buf_client 