#ifndef DATEANDTIME_H_
#define DATEANDTIME_H_
#include <iostream>
#include <string>
#include <stdio.h>
#include <time.h>
const std::string currentDateTime() {
    time_t now = time(0);
    struct tm tstruct;
    char buf[80];
    tstruct = *localtime(&now);
    strftime(buf, sizeof(buf), "Dated: %Y-%m-%d    Running Time : %X", &tstruct);
    return buf;
}

#endif // DATEANDTIME_H_