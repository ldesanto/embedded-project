#include "contiki.h"
#include <stdlib.h>
#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include <string.h>
#include <stdio.h> /* For printf() */
#include "cc2420.h"
#include "cc2420_const.h"
/* Log configuration */
#include "sys/log.h"

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Configuration */
#define WINDOW_SIZE 10 // window size in seconds
#define MAX_CANDIDATE 10 // max number of candidates
#define MAX_RETRIES 2 // max number of retries to find a parent
#define MAX_WAIT 10 // max wait time for a response from parent (in seconds)
#define MAX_CHILDREN 10 // max number of children

#define EDGE_NODE { { 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } } // edge node address

/*---------------------------------------------------------------------------*/
PROCESS(setup_process, "setup_process");
PROCESS(main_coordinator, "main_coordinator");
PROCESS(main_sensor, "main_coordinator");

AUTOSTART_PROCESSES(&setup_process);
static int last_poll = 0;
static int retries = 0;
static linkaddr_t coord_candidate[MAX_CANDIDATE];
static int coord_candidate_index = 0;
static linkaddr_t sensor_candidate[MAX_CANDIDATE];

static linkaddr_t children[MAX_CHILDREN];
static int children_size = 0;

static int sensor_candidate_index = 0;
static linkaddr_t parent = EDGE_NODE;
static int coord_candidate_rssi[MAX_CANDIDATE];
static int sensor_candidate_rssi[MAX_CANDIDATE];
static int type = -1; // 0: sensor, 1: coordinator // -1 undecided
static int counter = 0;
static const linkaddr_t edge_node = EDGE_NODE;
/*---------------------------------------------------------------------------*/

void new_child(const linkaddr_t* child) {
    // increase the size of the children array
    LOG_INFO("Adding child %d.%d\n", child->u8[0], child->u8[1]);
    memcpy(&children[children_size], child, sizeof(linkaddr_t));
    children_size++;
}

void input_callback(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest)
{
    static char message[20];
    static linkaddr_t source;
    memcpy(&source, src, sizeof(linkaddr_t));
    memcpy(&message, data, len);
    LOG_INFO("Received %s from %d.%d to %d.%d\n", message, src->u8[0], src->u8[1], dest->u8[0], dest->u8[1]);
    // if message is "coordinator", add src to coord_candidate
    if (strcmp(message, "coordinator") == 0) {
        memcpy(&coord_candidate[coord_candidate_index], &source, sizeof(linkaddr_t));
        coord_candidate_rssi[coord_candidate_index] = cc2420_last_rssi;
        coord_candidate_index++;
        return;
        
    }
    // if message is "sensor", add src to sensor_candidate
    else if (strcmp(message, "sensor") == 0) {
        memcpy(&sensor_candidate[sensor_candidate_index], &source, sizeof(linkaddr_t));
        sensor_candidate_rssi[sensor_candidate_index] = cc2420_last_rssi;
        sensor_candidate_index++;
        return;
    }
    // if message is "child", we are coordinator, add src to children
    else if (strcmp(message, "child") == 0) {
        // if we have no parent, set type as 1
        if (linkaddr_cmp(&parent, &linkaddr_null)) {
            type = 1;
            // broadcast "coordinator" to all other nodes
            memcpy(nullnet_buf, "coordinator", sizeof("coordinator"));
            nullnet_len = sizeof("coordinator");
            NETSTACK_NETWORK.output(NULL);
            memcpy(&parent, &source, sizeof(linkaddr_t));
        }
        // if we are not coordinator, send "no" to child
        if (type == 0) {
            memcpy(nullnet_buf, "no", sizeof("no"));
            nullnet_len = sizeof("no");
            NETSTACK_NETWORK.output(&source);
            return;
        } else if (type == 1){
            // add the child to children array
            new_child(&source);
            // send "parent" to child
            memcpy(nullnet_buf, "parent", sizeof("parent"));
            nullnet_len = sizeof("parent");
            NETSTACK_NETWORK.output(&source);
        } else {
            // if we are undecided, send "no" to child
            memcpy(nullnet_buf, "no", sizeof("no"));
            nullnet_len = sizeof("no");
            NETSTACK_NETWORK.output(&source);
        }
       
        return;
    }
    // if message is new, send our type
    else if (strcmp(message, "new") == 0) {
        if (type == 0) {
            memcpy(nullnet_buf, "sensor", sizeof("sensor"));
            nullnet_len = sizeof("sensor");
            NETSTACK_NETWORK.output(&source);
            return;
        }
        else if (type == 1){
            // if there is space for new child, send "coordinator"
            if (children_size < MAX_CHILDREN) {
                memcpy(nullnet_buf, "coordinator", sizeof("coordinator"));
                nullnet_len = sizeof("coordinator");
                NETSTACK_NETWORK.output(&source);
                return;
                
            }
            // else ignore the message
        }
    }
    else if (strcmp(message, "poll") == 0) {
        
        // set the last poll time
        last_poll = clock_seconds();

        // send back some random data
        memcpy(nullnet_buf, &counter, sizeof(counter));
        nullnet_len = sizeof(counter);
        counter++;
        NETSTACK_NETWORK.output(&source);
        return;
    }
    // if message is "parent", set src as parent
    else if (strcmp(message, "parent") == 0) {
        type = 0;
        return;
    }
    // if message is "no", restart the process
    else if (strcmp(message, "no") == 0) {
        // restart the process
        if (retries < MAX_RETRIES){
            retries++;
            process_exit(&setup_process);
            process_exit(&main_coordinator);
            process_exit(&main_sensor);
            process_start(&setup_process, NULL);
        } else {
            // if we have tried too many times, set type as 1
            type = 1;
            memcpy(&parent, &edge_node, sizeof(linkaddr_t));
        }
        return;
    }

}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(setup_process, ev, data)
{
    static struct etimer periodic_timer;
    static char message[20];
    PROCESS_BEGIN();
    LOG_INFO("Starting setup process\n");
    type = -1;
    /* Initialize NullNet */
    // empty all the arrays
    memset(&coord_candidate, 0, sizeof(coord_candidate));
    memset(&coord_candidate_rssi, 0, sizeof(coord_candidate_rssi));
    memset(&sensor_candidate, 0, sizeof(sensor_candidate));
    memset(&sensor_candidate_rssi, 0, sizeof(sensor_candidate_rssi));
    children_size = 0;
    coord_candidate_index = 0;
    sensor_candidate_index = 0;

    nullnet_buf = (uint8_t *)&message;
    nullnet_len = sizeof(message);
    nullnet_set_input_callback(input_callback);


    // wait for a random time between 0 and 10 seconds
    etimer_set(&periodic_timer, rand() % (10 * CLOCK_SECOND));
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));


    memcpy(nullnet_buf, "new", sizeof("new"));
    //memcpy(nullnet_buf, &message, sizeof(message));
    nullnet_len = sizeof(message);
    NETSTACK_NETWORK.output(NULL);

    // wait for 2 seconds
    etimer_set(&periodic_timer,2 * CLOCK_SECOND);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

    // if there is only one coordinator candidate, set it as parent
    if (coord_candidate_index == 1) {
        memcpy(&parent, &coord_candidate[0], sizeof(linkaddr_t));
        type = 0;

    }
    // if there are multiple coordinator candidates, set the one with highest rssi as parent
    else if (coord_candidate_index > 1) {
        int max_rssi = -100;
        int max_index = 0;
        for (int i = 0; i < coord_candidate_index; i++) {
            if (coord_candidate_rssi[i] > max_rssi) {
                max_rssi = coord_candidate_rssi[i];
                max_index = i;
            }
        }
        type = 0;
        memcpy(&parent, &coord_candidate[max_index], sizeof(linkaddr_t));
    }
    // if there is not coordinator candidate but there is sensor candidate, set the one with highest rssi as parent
    else if (sensor_candidate_index > 0) {
        int max_rssi = -100;
        int max_index = 0;
        for (int i = 0; i < sensor_candidate_index; i++) {
            if (sensor_candidate_rssi[i] > max_rssi) {
                max_rssi = sensor_candidate_rssi[i];
                max_index = i;
            }
        }
        type = 0;
        memcpy(&parent, &sensor_candidate[max_index], sizeof(linkaddr_t));
    }
    // if there is no coordinator candidate, set the edge node as parent
    else {
        memcpy(&parent, &edge_node, sizeof(linkaddr_t));
        type = 1;
        memcpy(nullnet_buf, "coordinator", sizeof("coordinator"));
        nullnet_len = sizeof("coordinator");
        NETSTACK_NETWORK.output(&parent); 
    }

    if (type == 0) {
        memcpy(nullnet_buf, "child", sizeof("child"));
        nullnet_len = sizeof("child");
        NETSTACK_NETWORK.output(&parent); 
    }
    
    // sleep for 2 seconds
    etimer_set(&periodic_timer, 2 * CLOCK_SECOND);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

    // if we are coordinator, start the main_coordinator process
    if (type == 1) {
        process_start(&main_coordinator, NULL);
    }
    // if we are sensor, start the main_sensor process
    else {
        process_start(&main_sensor, NULL);
    }

    PROCESS_END();
}

PROCESS_THREAD(main_coordinator, ev, data)
{
    PROCESS_BEGIN();
    static struct etimer periodic_timer;
    static char message[20];

    LOG_INFO("COORDINATOR | Parent: %d.%d\n", parent.u8[0], parent.u8[1]);

    /* Initialize NullNet */
    nullnet_buf = (uint8_t *)&message;
    nullnet_len = sizeof(message);
    nullnet_set_input_callback(input_callback);
    static int i;

    while (1){
        i=0;
        // sleep for 1 second
        etimer_set(&periodic_timer, CLOCK_SECOND);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
        
        // for each child, send "poll" to it then wait for window_size / num_children seconds
        while(i < children_size) {
            etimer_set(&periodic_timer, (WINDOW_SIZE * CLOCK_SECOND)/children_size);
            PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
            LOG_INFO("Sending poll message %d, children size %d\n", i, children_size);
            memcpy(nullnet_buf, "poll", sizeof("poll"));
            nullnet_len = sizeof("poll");
            NETSTACK_NETWORK.output(&children[i]);
            
            i++;
        }
    }
    
    PROCESS_END();
}


PROCESS_THREAD(main_sensor, ev, data)
{
    PROCESS_BEGIN();
    static struct etimer periodic_timer;
    static char message[20];
    LOG_INFO("SENSOR | Parent: %d.%d\n", parent.u8[0], parent.u8[1]);

    /* Initialize NullNet */
    nullnet_buf = (uint8_t *)&message;
    nullnet_len = sizeof(message);
    nullnet_set_input_callback(input_callback);
    while (1){
        etimer_set(&periodic_timer, 20 * CLOCK_SECOND);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer) || ev == PROCESS_EVENT_EXIT);
        if ( ev == PROCESS_EVENT_EXIT ) {
            LOG_INFO("Exiting main_sensor\n");
            break;
        }
        // check if last poll message was received within MAX_WAIT seconds
        if (last_poll + MAX_WAIT < clock_seconds()) {
            LOG_INFO("No poll message received within %d seconds\n", MAX_WAIT);
            // restart setup process
            process_start(&setup_process, NULL);
            break;
        }
    }
    
    PROCESS_END();
}