#include "badgevms/device.h"

#include <stdio.h>

#include <unistd.h>

int main(int argc, char *argv[]) {
    sleep(5);

    orientation_device_t *orientation;

    orientation = (orientation_device_t *)device_get("ORIENTATION0");

    if (orientation == NULL) {
        printf("Well, no device found");
        return 0;
    }

    printf("Get BMI270 accel...\n");
    int ret = orientation->_get_orientation(orientation);
    printf("Orientation: %d \n", ret);
    int degrees = orientation->_get_orientation_degrees(orientation);
    printf("Orientation degrees: %d \n", degrees);
}