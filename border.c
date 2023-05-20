#include "contiki.h"
#include <stdlib.h>
#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include <string.h>
#include <stdio.h>
#include "cc2420.h"
#include "cc2420_const.h"
#include "sys/log.h"

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Configuration */
#define WINDOW_SIZE 1 // window size in seconds
#define MAX_COORDINATOR 10 // maximum number of coordinators
#define MAX_SENSORS 50 // maximum number of sensors
#define WAIT_SYNC 5 // time to wait for synchronization


/*---------------------------------------------------------------------------*/
PROCESS(init, "Init");
PROCESS(setup_process, "Setup process");
PROCESS(collection_process, "Collection process");
PROCESS(synchronizaton, "synchronizaton");
PROCESS(timeslotting, "timeslotting");

AUTOSTART_PROCESSES(&init);

static linkaddr_t border_addr = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}};
static bool address_received = false; // flag to indicate if the address was received
static linkaddr_t last_sensor; // address of the last sensor from which a message was received
static linkaddr_t sensors[MAX_SENSORS]; // list of sensors addresses
static int number_of_sensors = 0; // number of sensors
static int count_of_sensors[MAX_SENSORS]; // list of counts of the sensors
static int last_count = 0; // count of the last sensor from which a message was received
static linkaddr_t coordinator_list[MAX_COORDINATOR]; // list of coordinators addresses
static linkaddr_t pending_list[MAX_COORDINATOR]; // list of pending coordinators addresses
static int number_of_coordinators = 0; // number of coordinators
static int number_of_pending = 0; // number of pending coordinators
static clock_time_t average_clock = 0; // average clock time of the coordinators
static bool waiting_for_sync = false; // flag to indicate if the node is waiting for synchronization
static int clock_received = 0; // number of clock times received
static clock_time_t coordinator_clock[MAX_COORDINATOR]; // clock times of the coordinators
static clock_time_t offset = 0;
static unsigned long timeslots[MAX_COORDINATOR]; // timeslots of the coordinators
static unsigned long timeslot_start[MAX_COORDINATOR] ; // start time of the timeslot
static int receiving_from = -1; // index of the coordinator from which the node is receiving
static int number_of_messages = 0; // number of messages received per window
static bool waiting_for_timeslot = false; // flag to indicate if the node is waiting for a timeslot
static int type = -1; // type of messsages active 1, inactive 0
static bool stop = false; // flag to indicate if the node should exit
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

void input_callback_collect(const void *data, uint16_t len,
  const linkaddr_t *src, const linkaddr_t *dest)
{
    static char message[20];
    static linkaddr_t source;
    memcpy(&source, src, sizeof(linkaddr_t));
    memcpy(message, data, len);

    if (strcmp(message, "coordinator") == 0){
        //a new coordinator arrived, add it to the list of pending coordinators
        if ((number_of_coordinators + number_of_pending) < MAX_COORDINATOR){
            LOG_INFO("Received coordinator message from %d.%d\n", source.u8[0], source.u8[1]);
            memcpy(&pending_list[number_of_pending], src, sizeof(linkaddr_t));
            number_of_pending++;
        }
        else {
            LOG_INFO("Maximum number of coordinators reached\n");
        }
        return;
    }
    else if (strcmp(message, "ping") == 0) {
        LOG_INFO("Received ping message from %d.%d\n", source.u8[0], source.u8[1]);
        number_of_messages ++;
        return;
    }
    else if (strcmp(message, "sensor") == 0){
        // a message from a new sensor is about to be forwarded
        LOG_INFO("Received sensor message from %d.%d\n", source.u8[0], source.u8[1]);
        address_received = false;
        number_of_messages ++;
        return;
    }
    else if (address_received){
        //if the address is already received, then we're listenning for the count
        LOG_INFO("Received count %s from %d.%d\n", message, last_sensor.u8[0], last_sensor.u8[1]);
        last_count = atoi(message);
        return;
    }
    else {
        //if the address is not received, then we're listenning for the address
        LOG_INFO("Received address %s from %d.%d\n", message, source.u8[0], source.u8[1]);
        memcpy(&last_sensor, src, sizeof(linkaddr_t));
        address_received = true;
        return;
    }
}
PROCESS_THREAD(collection_process, ev, data){
    static struct etimer timer;
    static char message[20];
    PROCESS_BEGIN()
    LOG_INFO("Collection process started\n");
    nullnet_buf = (uint8_t *)&message;
    nullnet_len = sizeof(message);
    nullnet_set_input_callback(input_callback_collect);

    while (1){
        etimer_set(&timer, CLOCK_SECOND * WINDOW_SIZE);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));
        LOG_INFO("Number of messages received: %d\n", number_of_messages);
        number_of_messages = 0;
        break;
    }
    PROCESS_END();
}
/*---------------------------------------------------------------------------*/
void input_callback_synchronization(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest){
    static char message[20];
    static linkaddr_t source;
    memcpy(&source, src, sizeof(linkaddr_t));
    memcpy(message, data, len);

    //wait for number of coordinators clock times to synchronize
    if (waiting_for_sync && strcmp(message, "new") != 0){
        LOG_INFO("Received clock from %d.%d\n", source.u8[0], source.u8[1]);
        //if time message received, mark the coordinator as active
        coordinator_clock[clock_received] = atoi(message);
        clock_received++;

        if (clock_received == number_of_coordinators){
            waiting_for_sync = false;
        }
        return;
    }

    if (strcmp(message, "coordinator") == 0){
        //a new coordinator arrived, add it to the list of pending coordinators
        if ((number_of_coordinators + number_of_pending) < MAX_COORDINATOR){
            LOG_INFO("Received coordinator message from %d.%d\n", source.u8[0], source.u8[1]);
            memcpy(&pending_list[number_of_pending], src, sizeof(linkaddr_t));
            number_of_pending++;
        }
        else {
            LOG_INFO("Maximum number of coordinators reached\n");
        }
        return;
    }
}

//create a process that send "clock_request" to all coordinatorrs, as well as the pending ones
PROCESS_THREAD(synchronizaton, ev, data)
{
    PROCESS_BEGIN();
    static struct etimer timer;
    static char message[20];
    LOG_INFO("Synchronization process started\n");

    nullnet_buf = (uint8_t *)&message;
    nullnet_len = sizeof(message);
    nullnet_set_input_callback(input_callback_synchronization);

    //free coordinator clock list
    memset(coordinator_clock, 0, sizeof(coordinator_clock));

    //send clock_request to all coordinators
    for (int i = 0; i < number_of_coordinators; i++){
        memcpy(nullnet_buf, "clock_request", sizeof("clock_request"));
        LOG_INFO("Sending clock_request to %d.%d\n", coordinator_list[i].u8[0], coordinator_list[i].u8[1]);
        NETSTACK_NETWORK.output(&coordinator_list[i]);
    }
    //send clock_request to all pending coordinators
    for (int i = 0; i < number_of_pending; i++){
        memcpy(nullnet_buf, "clock_request", sizeof("clock_request"));
        LOG_INFO("Sending clock_request to %d.%d\n", pending_list[i].u8[0], pending_list[i].u8[1]);
        NETSTACK_NETWORK.output(&pending_list[i]);
        //remove coordinator from the pending list and add it to the coordinator list
        memcpy(&coordinator_list[number_of_coordinators], &pending_list[i], sizeof(linkaddr_t));
        number_of_coordinators++;
    }
    waiting_for_sync = true;
    //wait for number of coordinators clock times to synchronize
    etimer_set(&timer, WAIT_SYNC * CLOCK_SECOND);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));

    //calculate average clock time
    for (int i = 0; i < number_of_coordinators; i++){
        average_clock += coordinator_clock[i];
    }
    average_clock += clock_time();
    average_clock /= (number_of_coordinators + 1);

    //calculate the offset between own clock and average clock
    offset = average_clock - clock_time();

    LOG_INFO("Sending new clocktime\n");
    //send synchronization message to all coordinators
    memcpy(nullnet_buf, &average_clock, sizeof(average_clock));
    NETSTACK_NETWORK.output(NULL);


    //free coordinator clock list
    memset(coordinator_clock, 0, sizeof(coordinator_clock));
    //free pending list
    memset(pending_list, 0, sizeof(pending_list));

    PROCESS_END();
}
/*---------------------------------------------------------------------------*/
void input_callback_timeslot(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest){
    static char message[20];
    static linkaddr_t source;
    memcpy(&source, src, sizeof(linkaddr_t));
    memcpy(message, data, len);

    if (strcmp(message, "coordinator") == 0){
        //a new coordinator arrived, add it to the list of pending coordinators
        if (number_of_coordinators < MAX_COORDINATOR){
            LOG_INFO("Received coordinator message from %d.%d\n", source.u8[0], source.u8[1]);
            memcpy(&pending_list[number_of_pending], src, sizeof(linkaddr_t));
            number_of_pending++;
        }
        else {
            LOG_INFO("Maximum number of coordinators reached\n");
        }
        return;
    }
}

PROCESS_THREAD(timeslotting, ev, data){
    PROCESS_BEGIN();
    LOG_INFO("Timeslot process started\n");
    static unsigned long delay = 1;
    static char message[20];
    waiting_for_timeslot = true;

    nullnet_buf = (uint8_t *)&message;
    nullnet_len = sizeof(message);
    nullnet_set_input_callback(input_callback_timeslot);
    //divide the window in timeslots
    for (int i = 0; i < number_of_coordinators; i++){
        timeslots[i] = (WINDOW_SIZE) / number_of_coordinators;
    }

    //calculate the start of timeslots for each coordinator
    for (int i = 0; i < number_of_coordinators; i++){
        if (i == 0){
            timeslot_start[i] = 0;
        }
        else{
            timeslot_start[i] = timeslot_start[i-1] + timeslots[i-1];
        }
    }

    //add average_clock and delay to timeslot_start
    for (int i = 0; i < number_of_coordinators; i++){
        timeslot_start[i] += (average_clock + delay) ;
    }
    //send timeslot_start to all coordinators
    for (int i = 0; i < number_of_coordinators; i++){
        memcpy(nullnet_buf, "window", sizeof("window"));
        NETSTACK_NETWORK.output(&coordinator_list[i]);
        memcpy(&timeslot_start[i], nullnet_buf, sizeof(timeslot_start[i]));
        LOG_INFO("Sending timeslot_start to %d.%d\n", coordinator_list[i].u8[0], coordinator_list[i].u8[1]);
        NETSTACK_NETWORK.output(&coordinator_list[i]);
    }
    //send timeslot to all coordinators
    for (int i = 0; i < number_of_coordinators; i++){
        memcpy(nullnet_buf, &timeslots[i], sizeof(timeslots[i]));
        LOG_INFO("Sending timeslot to %d.%d\n", coordinator_list[i].u8[0], coordinator_list[i].u8[1]);
        NETSTACK_NETWORK.output(&coordinator_list[i]);
    }
    waiting_for_timeslot = false;
    PROCESS_END();
}

/*---------------------------------------------------------------------------*/
void input_callback_setup(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest){
    static char message[20];
    static linkaddr_t source;
    memcpy(&source, src, sizeof(linkaddr_t));
    memcpy(message, data, len);
    LOG_INFO("Received message from %d.%d: %s\n", source.u8[0], source.u8[1], message);
    process_poll(&setup_process);

    if (strcmp(message, "coordinator") == 0){
        //a new coordinator arrived, add it to the list of pending coordinators
        if ((number_of_coordinators + number_of_pending) < MAX_COORDINATOR){
            LOG_INFO("Received coordinator message from %d.%d\n", source.u8[0], source.u8[1]);
            memcpy(&pending_list[number_of_pending], src, sizeof(linkaddr_t));
            number_of_pending++;
        }
        else {
            LOG_INFO("Maximum number of coordinators reached\n");
        }
    }
    //if the message is stop, stop the process
    if (strcmp(message, "stop") == 0){
        LOG_INFO("Received stop message\n");
        stop = true;
    }
    if (strcmp(message, "ping") == 0){
        LOG_INFO("Received ping message\n");
    }

}

PROCESS_THREAD(setup_process, ev, data){
    PROCESS_BEGIN();
    LOG_INFO(" set up process started\n");

    static struct etimer timer;
    static char message[20];
    static int inc = 0;

    nullnet_buf = (uint8_t *)&message;
    nullnet_len = sizeof(message);

    nullnet_set_input_callback(input_callback_setup);
    etimer_set(&timer, CLOCK_SECOND * 5);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));

    if(number_of_pending == 0){
        process_start(&init, NULL);
    }

    while(!stop){
        //start synchronization
        process_start(&synchronizaton, NULL);
        //wait for synchronization to finish
        PROCESS_WAIT_EVENT_UNTIL(waiting_for_sync == false);
        //start timeslotting
        process_start(&timeslotting, NULL);
        //wait for timeslotting to finish
        PROCESS_WAIT_EVENT_UNTIL(waiting_for_timeslot == false);
        //start the listenning process

        while( inc < number_of_coordinators){
            //wait for the start of the timeslot
            etimer_set(&timer, timeslot_start[inc] - (clock_time() + offset));
            PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));
            if (inc == 0){
                receiving_from = 0;
                process_start(&collection_process, NULL);
            }
            etimer_set(&timer, timeslot_start[inc] + timeslots[inc] - (clock_time() + offset));
            PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));
            receiving_from ++;
            inc++;
        }
        //stop the collecting process
        process_exit(&collection_process);
        //empty all the list but the list_of_coordinators, number of coordinators, and number of pending
        memset(&pending_list, 0, sizeof(pending_list));
        memset(&timeslot_start, 0, sizeof(timeslot_start));
        memset(&timeslots, 0, sizeof(timeslots));
        memset(&average_clock, 0, sizeof(average_clock));
        memset(&offset, 0, sizeof(offset));
        receiving_from = -1;
        type = -1;
        waiting_for_sync = false;
        waiting_for_timeslot = false;
        LOG_INFO("new loop");

    }
    LOG_INFO("Stopping the process");
    PROCESS_END();
}

void input_callback_init(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest){
    static char message[20];
    static linkaddr_t source;
    memcpy(&source, src, sizeof(linkaddr_t));
    memcpy(message, data, len);
    LOG_INFO("INIT | Received message from %d.%d: %s\n", source.u8[0], source.u8[1], message);

    if (strcmp(message, "coordinator") == 0){
        //a new coordinator arrived, add it to the list of pending coordinators
        if ((number_of_coordinators + number_of_pending) < MAX_COORDINATOR){
            LOG_INFO("Received coordinator message from %d.%d\n", source.u8[0], source.u8[1]);
            memcpy(&pending_list[number_of_pending], src, sizeof(linkaddr_t));
            number_of_pending++;
        }
        else {
            LOG_INFO("Maximum number of coordinators reached\n");
        }
    }
    process_poll(&init);
}

PROCESS_THREAD(init, ev, data){
    PROCESS_BEGIN();
    linkaddr_set_node_addr(&border_addr);
    LOG_INFO("INIT | init process started with address %d%d\n", linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1]);

    static char message[20];

    nullnet_buf = (uint8_t *)&message;
    nullnet_len = sizeof(message);
    nullnet_set_input_callback(input_callback_init);

    //send a message to all the nodes to start the setup process
    memcpy(message, "border", sizeof("border"));
    for (int i = 0; i < 20; i++){
        memcpy(nullnet_buf, &message, sizeof(message));
        NETSTACK_NETWORK.output(NULL);
    }
    PROCESS_WAIT_EVENT_UNTIL(number_of_pending > 0 || ev == PROCESS_EVENT_POLL) ;
    process_start(&setup_process, NULL);
    PROCESS_END();
}