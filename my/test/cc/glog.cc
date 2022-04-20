//  Usage: g++ test/cc/glog2.cc -o glogtest -lglog
// ./glogtest 4 logs
//  reference: * https://blog.csdn.net/Solomon1558/article/details/52558503
//             * https://zhuanlan.zhihu.com/p/26025722

#include <iostream>
#include <string>
#include <glog/logging.h>

int main(int argc, char** argv) {
    FLAGS_alsologtostderr = 1;
    google::InitGoogleLogging(argv[0]);

    //通过SetLogDestination可能没有设置log_dir标志位的方式方便(会遗漏一些日志)
    //google::SetLogDestination(google::GLOG_INFO, "/tmp/today");

    //标志位
    FLAGS_colorlogtostderr=true;  //设置输出颜色
    FLAGS_v = std::atoi(argv[1]); //设置最大显示等级(超过该等级将不记录log)
    FLAGS_log_dir = "logs";

    LOG(INFO) << "Found " << google::COUNTER << " arguments!";

    // assert 检测文件是否存在
    CHECK(access(argv[2], 0) != -1) << "No such file: "<<argv[2];

    LOG(INFO) << "I am INFO!";
    LOG(WARNING) << "I am WARNING!";
    LOG(ERROR) << "I am ERROR!";

    // LOG_IF(INFO, num_cookies > 10) << "Got lots of cookies"; //当条件满足时输出日志
    LOG_EVERY_N(INFO, 10) << "Got the " << google::COUNTER << "th cookie"; //第一次执行以后每隔十次记录一次log
    // LOG_IF_EVERY_N(INFO, (size > 1024), 10) //上面两者的结合
    LOG_FIRST_N(INFO, 20); // 此语句执行的前20次都输出日志；后面执行不输出日志


    //VLOG用来自定义日志, 可以在括号内指定log级别
    VLOG(1) << "[Custom log(VLOG)] Level 1!";
    VLOG(2) << "[Custom log(VLOG)] Level 2!";
    VLOG(3) << "[Custom log(VLOG)] Level 3!";
    VLOG(4) << "[Custom log(VLOG)] Level 4! This is used for detailed message which need not to be printed each time.";
    VLOG(5) << "[Custom log(VLOG)] Level 5! On this level messages are print as detail as possible for debugging.";
    LOG(FATAL) << "I am FATAL!";
    return 0;
}