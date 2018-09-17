// Copyright (C) 2018, Jaguar Land Rover
// This program is licensed under the terms and conditions of the
// Mozilla Public License, version 2.0.  The full text of the 
// Mozilla Public License is at https://www.mozilla.org/MPL/2.0/
//
// Author: Magnus Feuer (mfeuer1@jaguarlandrover.com)
//
// Running example code from README.md in https://github.com/PDXOSTC/dstc
//

#include <stdio.h>
#include <stdlib.h>
#include "dstc.h"

// Generate deserializer for multicast packets sent by dstc_message()
// above.
// The deserializer decodes the incoming data and calls the
// print_name_and_age() function in this file.
//
DSTC_SERVER(print_name_and_age, char, [32], int,)

//
// Print out name and age.
// Invoked by deserilisation code generated by DSTC_SERVER() above.
// Please note that the arguments must match between the function below
// and the macro above.
//
void print_name_and_age(char name[32], int age)
{
    printf("Name: %s\n", name);
    printf("Age:  %d\n", age);
}

int main(int argc, char* argv[])
{
    while(1) {
        dstc_read();
    }
        
    exit(0);
}

