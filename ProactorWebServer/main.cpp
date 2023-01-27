#include <stdio.h>
#include <stdlib.h>
#include "epoll_class.h"

int main()
{
    int port = 80;

    epoll_class demo(port);
    demo.run();

    
    return 0;
}