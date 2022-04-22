
#include <iostream>
#include <string>
#include <glog/logging.h>


#include <limits.h>
#include <float.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

/* Byte ordering detection */
#include <sys/types.h> /* This will likely define BYTE_ORDER */

#include <string>
#include <utility>

// byte不是一种新类型，在C++中byte被定义的是unsigned char类型；但在C#里面byte被定义的是unsigned int类型

//int转byte
void  intToByte(int i,byte *bytes,int size = 4)

{
     //byte[] bytes = new byte[4];
    memset(bytes,0,sizeof(byte) *  size);
    bytes[0] = (byte) (0xff & i);
    bytes[1] = (byte) ((0xff00 & i) >> 8);
    bytes[2] = (byte) ((0xff0000 & i) >> 16);
    bytes[3] = (byte) ((0xff000000 & i) >> 24);
    return ;
 }

//byte转int
 int bytesToInt(byte* bytes,int size = 4) 
{
    int addr = bytes[0] & 0xFF;
    addr |= ((bytes[1] << 8) & 0xFF00);
    addr |= ((bytes[2] << 16) & 0xFF0000);
    addr |= ((bytes[3] << 24) & 0xFF000000);
    return addr;
 }
 

// 取1个字节出来比较：
// if( (0xff & A) ==TYPE) 

// unsigned int(uint32_t)大小端转换函数
unsigned int BLEndianUint32(unsigned int value)
{
    return 
        ((value & 0x000000FF) << 24) |  
        ((value & 0x0000FF00) << 8) |  
        ((value & 0x00FF0000) >> 8) | 
        ((value & 0xFF000000) >> 24);
}


// float大小端转换函数
// 由于Float类型的数据在计算中保存方法不一样， 所以对Float类型的数据做大小端转换的情况不能简单的通过宏移位来完成。
typedef union FLOAT_CONV
{
    float f;
    char c[4];
}float_conv;
float BLEndianFloat(float value)
{
    float_conv d1,d2;
    d1.f = value;
    d2.c[0] = d1.c[3];
    d2.c[1] = d1.c[2];
    d2.c[2] = d1.c[1];
    d2.c[3] = d1.c[0];
    return d2.f;
}

// unsigned short大小端转换函数
unsigned short BLEndianUshort(unsigned short value)
{
    return ((value & 0x00FF) << 8 ) | ((value & 0xFF00) >> 8);
}




int main(int argc, char** argv) {
    FLAGS_alsologtostderr = 1;
    google::InitGoogleLogging(4);

    //标志位
    FLAGS_colorlogtostderr=true;  //设置输出颜色
    FLAGS_v = std::atoi(argv[1]); //设置最大显示等级(超过该等级将不记录log)
    FLAGS_log_dir = "logs";

    LOG(INFO) << "I am INFO!";
    LOG(WARNING) << "I am WARNING!";
    LOG(ERROR) << "I am ERROR!";

    char buf[1];
    buf[0] = static_cast<uint8_t>(8 & 0xff);

    uint8_t value=static_cast<uint8_t>(buf[0] & 0xff);

    LOG(INFO) << value;

    return 0;
}