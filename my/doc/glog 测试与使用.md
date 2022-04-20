glog 测试与使用

目录
1.概括
2. 四个等级
3.日志格式
4.设置参数
5.条件
6.例子1
7.例子2
1.概括

GLog 是一个应用程序级的日志记录的库，它提供了基于C++样式流和各种帮助程序宏的日志记录API,你可以很简单的将信息传输到LOG来记录消息。
官方git https://github.com/google/glog

2. 四个等级

GLog允许你制定消息的严重程度，一共有四个级别:INFO, WARNING, ERROR, FATAL。记录FATAL消息会终止程序(在记录消息之后),在GLog记录的消息的开头以I, W, E, F来标示信息的严重级别；

enum SeverityLevel
{
　　google::INFO = 0,
　　google::WARNING = 1,
　　google::ERROR = 2,
　　google::FATAL = 3,
};
3.日志格式

默认情况下日志会写入本地/tmp/文件夹中，文件名格式：...log..-.;
比如我实际项目中记录的文件名project_xxx.public-desktop.invalid-user.log.INFO.20211115-161541.1467
project_xxx.public-desktop.invalid-user.log.WARNING.20211115-153838.1771

4.设置参数

google::SetLogDestination(google::GLOG_INFO, "log/prefix_"); //设置特定严重级别的日志的输出目录和前缀。第一个参数为日志级别，第二个参数表示输出目录及日志文件名前缀
google::SetLogFilenameExtension("logExtension"); //在日志文件名中级别后添加一个扩展名。适用于所有严重级别
google::SetStderrLogging(google::GLOG_INFO); //大于指定级别的日志都输出到标准输出
FLAGS_logtostderr = true; //设置日志消息是否转到标准输出而不是日志文件
FLAGS_alsologtostderr = true; //设置日志消息除了日志文件之外是否去标准输出
FLAGS_colorlogtostderr = true; //设置记录到标准输出的颜色消息（如果终端支持）
FLAGS_log_prefix = true; //设置日志前缀是否应该添加到每行输出
FLAGS_logbufsecs = 0; //设置可以缓冲日志的最大秒数，0指实时输出
FLAGS_max_log_size = 10; //设置最大日志文件大小（以MB为单位）
FLAGS_stop_logging_if_full_disk = true; //设置是否在磁盘已满时避免日志记录到磁盘
FLAGS_log_dir = "./"; // 将日志文件输出到本地

在实际项目环境使用中，参数也可以命令行传入：

./main -alsologtostderr 1 -log_dir ../log -num ${TEST_NUM} -test_list ${TEST_LIST} -threads ${USE_THREAD}
这里的alsologtostderr和log_dir就是glog所需要的参数。

5.条件

这里在caffe源码大量使用

#define CHECK_EQ(val1, val2) CHECK_OP(_EQ, ==, val1, val2)
#define CHECK_NE(val1, val2) CHECK_OP(_NE, !=, val1, val2)
#define CHECK_LE(val1, val2) CHECK_OP(_LE, <=, val1, val2)
#define CHECK_LT(val1, val2) CHECK_OP(_LT, < , val1, val2)
#define CHECK_GE(val1, val2) CHECK_OP(_GE, >=, val1, val2)
#define CHECK_GT(val1, val2) CHECK_OP(_GT, > , val1, val2)
例子

 //有条件地中止程序
/   int a1 = 5;
    CHECK(a1 == 4) << "a1 != 4,fail!"; //a1 != 4的时候输出后面的打印，然后中止程序退出

    int a2 = 3;
    int a3 = 3;
    CHECK_EQ(a2,a3)<<"not  equal";//a2==a3的时候才继续运行  当a2！=a3的时候输出后面的打印退出中止运行
6.例子1

FLAGS_log_dir = "c:\\Logs";
google::InitGoogleLogging(argv[0]);
google::SetLogDestination(google::GLOG_INFO, "c:\\Logs\\INFO_");
google::SetStderrLogging(google::GLOG_INFO);
google::SetLogFilenameExtension("log_");
FLAGS_colorlogtostderr = true;  // Set log color
FLAGS_logbufsecs = 0;  // Set log output speed(s)
FLAGS_max_log_size = 1024;  // Set max log file size
FLAGS_stop_logging_if_full_disk = true;  // If disk is full
char str[20] = "hello log!";
LOG(INFO) << str;
CStringA cStr = "hello google!";
LOG(INFO) << cStr;
LOG(INFO) << "info test" << "hello log!";  //输出一个Info日志
LOG(WARNING) << "warning test";  //输出一个Warning日志
LOG(ERROR) << "error test";  //输出一个Error日志
google::ShutdownGoogleLogging();
7.例子2

CMakeLists.txt

cmake_minimum_required(VERSION 3.0 FATAL_ERROR)
project(test_glogs)
 
SET(CMAKE_BUILD_TYPE Debug)
 
#glog
include_directories(/data_2/lib/glog/include/)
link_directories(/data_2/lib/glog/lib)

add_executable(main main.cpp)
target_link_libraries(main glog)
set_property(TARGET main PROPERTY CXX_STANDARD 11)
main.cpp

#include <iostream>
#include <glog/logging.h>

int main(int argc, char** argv)
{

//    FLAGS_logtostderr = 1; // 将使日志信息记录到stderr而不保存到本地日志文件中，即使你设置了FLAGS_log_dir;

    FLAGS_alsologtostderr = true; //除了日志文件之外是否需要标准输出

    google::SetLogDestination(google::GLOG_WARNING, "./log/log_warning_"); //设置 google::WARNING 级别的日志存储路径和文件名前缀
    google::SetLogDestination(google::GLOG_INFO, "./log/log_info_"); //设置 google::INFO 级别的日志存储路径和文件名前缀

    google::InitGoogleLogging("test_2022");//初始化
//    google::SetLogDestination(google::GLOG_INFO,"./log/aTestInfo");//设置日志文件路径，默认+时间作为生成的日志文件名


    LOG(INFO) << "info test";  //输出一个Info日志


    //有条件地中止程序
//    int a1 = 5;
//    CHECK(a1 == 4) << "a1 != 4,fail!"; //a1 != 4的时候输出后面的打印，然后中止程序退出

    int a2 = 3;
    int a3 = 3;
    CHECK_EQ(a2,a3)<<"---==";//a2==a3的时候才继续运行  当a2！=a3的时候输出后面的打印退出中止运行



    LOG(WARNING) << "warning test";  //输出一个Warning日志
    LOG(ERROR) << "error test";  //输出一个Error日志
//    LOG(FATAL) << "fatal test";  //输出一个Fatal日志，这是最严重的日志并且输出之后会中止程序

    std::cout<<"-------end-----"<<std::endl;
    return 0;
}
但是这里有个问题就是warning的单独输出了到一个日志文件中，
但是info的里面同时输出了warning和info的信息。