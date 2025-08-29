#include <unistd.h>
#include "code/webserver.h"

int main() {
    WebServer server(
        1316, 3, 60000, false,             
        3306, "root", "@Hit2025", "UIEwebserver", 
        12, 6, true, 1, 1024); 
    server.Start();
}