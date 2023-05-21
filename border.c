#include "contiki.h"
#include <stdlib.h>
#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include <string.h>
#include <stdio.h>
#include "cc2420.h"
#include "cc2420_const.h"
#include "sys/log.h"
#include "dev/slip.h"
#include "dev/serial-line.h"
#include "cpu/msp430/dev/uart0.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Configuration */

#define WINDOW_SIZE 2000 // window size in milliseconds
#define MAX_COORDINATOR 4 // maximum number of coordinators
#define MAX_SENSORS  16// maximum number of sensors
#define WAIT_SYNC 1000 // time to wait for synchronization
#define DELAY 1000 // delay between messages

/*---------------------------------------------------------------------------*/

PROCESS(init, "Init");
AUTOSTART_PROCESSES(&init);

/*---------------------------------------------------------------------------*/

static bool address_received = false; // flag to indicate if the address of a sensor was received
static linkaddr_t last_sensor; // address of the last sensor from which a message was received
static linkaddr_t sensors[MAX_SENSORS]; // list of sensors addresses
static int number_of_sensors = 0; // number of sensors
static int count_of_sensors[MAX_SENSORS]; // list of counts of the sensors
static int last_count = 0; // count of the last sensor from which a message was received
static linkaddr_t coordinator_list[MAX_COORDINATOR]; // list of coordinators addresses
static linkaddr_t pending_list[MAX_COORDINATOR]; // list of pending coordinators addresses
static int number_of_coordinators = 0; // number of coordinators
static int number_of_pending = 0; // number of pending coordinators
static uint32_t average_clock = 0; // average clock time of the coordinators
static bool waiting_for_sync = false; // flag to indicate if the node is waiting for synchronization
static int clock_received = 0; // number of clock times received
static uint32_t coordinator_clock[MAX_COORDINATOR]; // clock times of the coordinators
static uint32_t offset = 0; //offset of the border with the calculated average clock
static uint32_t timeslots[MAX_COORDINATOR]; // timeslots of the coordinators
static uint32_t timeslot_start[MAX_COORDINATOR] ; // start time of the timeslot
static int receiving_from = -1; // index of the coordinator from which the node is receiving
static int number_of_messages = 0; // number of messages received per window
static bool stop = false; // flag to indicate if the node should exit
static int state = -1; // 0 : setup, 1 : synchronization, 2 : timeslotting, 3 : collection

/*---------------------------------------------------------------------------*/
void assign_last_counts() {
    //if a sensor is not in the list, add it
    for (int i = 0; i <= number_of_sensors; i++) {
        if (linkaddr_cmp(&sensors[i], &last_sensor)) {
            count_of_sensors[i] = last_count;
        }
        if (i == number_of_sensors - 1) {
            memcpy(&sensors[number_of_sensors], &last_sensor, sizeof(linkaddr_t));
            count_of_sensors[number_of_sensors] = last_count;
            number_of_sensors++;
        }
        if (number_of_sensors == 0) {
            memcpy(&sensors[number_of_sensors], &last_sensor, sizeof(linkaddr_t));
            count_of_sensors[number_of_sensors] = last_count;
            number_of_sensors++;
        }
    }
}

void send_sensor_data(){
    for (int i = 0; i <= number_of_sensors; i++) {
        uart0_writeb((unsigned char) i);
        uart0_writeb((unsigned char) count_of_sensors[i]);
    }
}

void input_callback(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest) {
    static char message[20];
    static linkaddr_t source;
    memcpy(&source, src, sizeof(linkaddr_t));
    memcpy(message, data, len);
    LOG_INFO("BORDER | Received message from %d.%d: '%s'\n", source.u8[0], source.u8[1], message);
    if (strcmp(message, "coordinator") == 0){
        //a new coordinator arrived, add it to the list of pending coordinators
        if ((number_of_coordinators + number_of_pending) < MAX_COORDINATOR){
            LOG_INFO("BORDER | Received coordinator message from %d.%d\n", source.u8[0], source.u8[1]);
            memcpy(&pending_list[number_of_pending], src, sizeof(linkaddr_t));
            number_of_pending++;
            LOG_INFO("BORDER | Number of pending coordinators: %d\n", number_of_pending);
            process_poll(&init);
        }
        else {
            LOG_INFO("BORDER | Maximum number of coordinators reached\n");
        }
    }
    else if (strcmp(message, "ping") == 0){
        //a ping message was received from a coordinator without a sensor
        LOG_INFO("BORDER | Received ping message from %d.%d\n", source.u8[0], source.u8[1]);
        number_of_messages++;
    }
    else if (strcmp(message, "sensor") == 0){
        LOG_INFO("BORDER | received sensor message from %d.%d\n", source.u8[0], source.u8[1]);
        address_received = false;
        number_of_messages++;
    }
    else if (strcmp(message, "stop") == 0){
        LOG_INFO("BORDER | received stop message from %d.%d\n", source.u8[0], source.u8[1]);
        stop = true; // stop the border
    }
    else{
        if(waiting_for_sync && (strcmp(message,"new") != 0)){
            //if the node is waiting for synchronization and the message is not a new message, it means that the message is a clock time
            LOG_INFO("BORDER | Received clock time from %d.%d\n", source.u8[0], source.u8[1]);
            memcpy(&coordinator_clock[clock_received], message, sizeof(uint32_t));
            clock_received++;
            if(clock_received == number_of_coordinators){
                waiting_for_sync = false;
                LOG_INFO("BORDER | Received all clock times\n");
            }
            process_poll(&init);
        }
        else if(address_received && (strcmp(message,"new") != 0)){
            //if the address is already received , then the message is the count
            LOG_INFO("BORDER | Received count from %d.%d\n", last_sensor.u8[0], last_sensor.u8[1]);
            last_count = atoi(message);
        }
        else if(!address_received && (strcmp(message,"new") != 0)){
            //if the message is a new message, then the next message will be the address
            LOG_INFO("BORDER | Received address %s from %d.%d\n", message, source.u8[0], source.u8[1]);
            memcpy(&last_sensor, src, sizeof(linkaddr_t));
            address_received = true;
        }
    }
}

void synchronization(){
    LOG_INFO("BORDER | starting synchronization\n");
    state = 1;
    memset(coordinator_clock, 0, sizeof(coordinator_clock));
    //send clock_request to all coordinators
    for (int i = 0; i < number_of_coordinators; i++){
        memcpy(nullnet_buf, "clock_request", sizeof("clock_request"));
        LOG_INFO("BORDER | Sending clock_request to %d.%d\n", coordinator_list[i].u8[0], coordinator_list[i].u8[1]);
        nullnet_len = sizeof("clock_request");
        NETSTACK_NETWORK.output(&coordinator_list[i]);
    }
    //send clock_request to all pending coordinators
    for (int i = 0; i < number_of_pending; i++){
        memcpy(nullnet_buf, "clock_request", sizeof("clock_request"));
        LOG_INFO("BORDER | Sending clock_request to %d.%d\n", pending_list[i].u8[0], pending_list[i].u8[1]);
        nullnet_len = sizeof("clock_request");
        NETSTACK_NETWORK.output(&pending_list[i]);
        //remove coordinator from the pending list and add it to the coordinator list
        memcpy(&coordinator_list[number_of_coordinators], &pending_list[i], sizeof(&pending_list[i]));
        number_of_coordinators++;
    }
    waiting_for_sync = true;
    number_of_pending = 0;
    memset(&pending_list, 0, sizeof(pending_list));

}

void timeslotting() {
    LOG_INFO("BORDER | starting timeslotting\n");
    state = 2;
    //divide the window into timeslots
    for (int i = 0; i < number_of_coordinators; i++){
        timeslots[i] = WINDOW_SIZE / number_of_coordinators;
    }
    //calculate the start of each timeslot
    for (int i = 0; i < number_of_coordinators; i++){
        timeslot_start[i] = (i * timeslots[i]) + clock_time() + DELAY + offset;
        LOG_INFO("BORDER | timeslot %d starts at %d\n", i, (int)timeslot_start[i]);
    }
}

void sendTimeslots(){
    static int i;
    static linkaddr_t coordinator;
    for (i = 0; i < number_of_coordinators; i++){
        coordinator = coordinator_list[i];
        memcpy(nullnet_buf, &timeslot_start[i], sizeof(timeslot_start[i]));
        LOG_INFO("BORDER | Sending timeslot to %d.%d\n", coordinator.u8[0], coordinator.u8[1]);

        //sending window message
        memcpy(nullnet_buf, "window", sizeof("window"));
        nullnet_len = sizeof("window");
        NETSTACK_NETWORK.output(&coordinator);

        //sending timeslot_start
        memcpy(nullnet_buf, &timeslot_start[i], sizeof(timeslot_start[i]));
        nullnet_len = sizeof(timeslot_start[i]);
        NETSTACK_NETWORK.output(&coordinator);

        //sending timeslot
        memcpy(nullnet_buf, &timeslots[i], sizeof(timeslots[i]));
        nullnet_len = sizeof(timeslots[i]);
        NETSTACK_NETWORK.output(&coordinator);
    }
}

PROCESS_THREAD(init, ev, data){
    //Main process

    PROCESS_BEGIN();
    uart0_set_input(serial_line_input_byte);
    LOG_INFO("BORDER | init process started with address %d%d\n", linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1]);
    static char message[20];
    static struct etimer timer;
    nullnet_buf = (uint8_t *)&message;
    nullnet_len = sizeof(message);
    nullnet_set_input_callback(input_callback);
    //send a message to all the nodes to start the setup process
    state = 0;
    LOG_INFO("BORDER | broadcasting border message\n");
    memcpy(message, "border", sizeof("border"));
    for (int i = 0; i < 20; i++){
        memcpy(nullnet_buf, &message, sizeof(message));
        nullnet_len = sizeof(message);
        NETSTACK_NETWORK.output(NULL);
    }
    PROCESS_WAIT_EVENT_UNTIL(number_of_coordinators > 0 || number_of_pending > 0);
    while(!stop){
        synchronization();
        average_clock = 0;
        LOG_INFO("BORDER | Waiting for clock\n");
        PROCESS_WAIT_EVENT_UNTIL(!waiting_for_sync || ev == PROCESS_EVENT_POLL);
        //calculate average clock time
        for (int i = 0; i < number_of_coordinators; i++){
            average_clock += (uint32_t) coordinator_clock[i];
        }
        average_clock += (uint32_t) clock_time();
        // log the number of coordinators
        average_clock = average_clock/(number_of_coordinators + 1);
        //calculate the offset between own clock and average clock
        offset = (uint32_t) (average_clock - clock_time());
        LOG_INFO("BORDER | Sending new clocktime (%d, %d)\n", (int) clock_time(), (int) average_clock);
        memcpy(nullnet_buf, &average_clock, sizeof(average_clock));
        nullnet_len = sizeof(average_clock);
        NETSTACK_NETWORK.output(NULL);

        //free coordinator clock list
        memset(coordinator_clock, 0, sizeof(coordinator_clock));
        clock_received = 0;
        //free pending list
        memset(pending_list, 0, sizeof(pending_list));
        LOG_INFO("BORDER | synchronization finished\n");
        //wait 1000 ticks
        etimer_set(&timer,WAIT_SYNC);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer) || ev == PROCESS_EVENT_POLL);
        //start timeslotting
        timeslotting();
        
        LOG_INFO("BORDER | Sending window slots\n");
        sendTimeslots();
        LOG_INFO("BORDER | Sending sensor data\n");
        send_sensor_data();

        //wait until the first timeslot starts
        LOG_INFO("BORDER | Waiting for timeslot, %d ticks\n", (int) (timeslot_start[0] - (clock_time() + offset)));
        // log the timeslot start, the current clock time and the offset
        LOG_INFO("BORDER | Timeslot start: %d, clock: %d, offset: %d\n", (int) timeslot_start[0], (int) clock_time(), (int) offset);
        etimer_set(&timer, (int) (timeslot_start[0] - (clock_time() + offset)));
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));
        state = 3;
        LOG_INFO("BORDER | Starting window\n");
        //update the receiving from coordinator list
        static int i2 = 0;
        while (i2 < number_of_coordinators){
            receiving_from = i2;
            LOG_INFO("BORDER | Receiving from %d.%d\n", coordinator_list[i2].u8[0], coordinator_list[i2].u8[1]);
            etimer_set(&timer, timeslots[i2]);
            LOG_INFO("BORDER | Waiting for %d ticks (%d, %d)\n", (int) timeslots[i2], i2, number_of_coordinators);
            PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));
            number_of_messages = 0;
            i2++;
        }
        LOG_INFO("BORDER | Window finished\n");
        state = -1;
    }
    PROCESS_END();
}

