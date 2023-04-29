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
#define MAX_CANDIDATE 10
#define MAX_CHILDREN 10

/*---------------------------------------------------------------------------*/
PROCESS(setup_process, "setup_process");
PROCESS(main_coordinator, "main_coordinator");
PROCESS(main_sensor, "main_coordinator");

AUTOSTART_PROCESSES(&setup_process);


static linkaddr_t edge_node = { { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } };
static linkaddr_t coord_candidate[MAX_CANDIDATE];
static int coord_candidate_index = 0;
static linkaddr_t sensor_candidate[MAX_CANDIDATE];

static linkaddr_t children[MAX_CHILDREN];
static int children_size = 0;

static int sensor_candidate_index = 0;
static linkaddr_t parent;
static int coord_candidate_rssi[MAX_CANDIDATE];
static int sensor_candidate_rssi[MAX_CANDIDATE];
static int type = 0; // 0: sensor, 1: coordinator
static int counter = 0;
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
        }
        // add the child to children array
        new_child(&source);
        // send "parent" to child
        memcpy(nullnet_buf, "parent", sizeof("parent"));
        nullnet_len = sizeof("parent");
        NETSTACK_NETWORK.output(&source);

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
        else {
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

}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(setup_process, ev, data)
{
    static struct etimer periodic_timer;
    static char message[20] = "new";
    parent = linkaddr_null;
    PROCESS_BEGIN();

    /* Initialize NullNet */
    nullnet_buf = (uint8_t *)&message;
    nullnet_len = sizeof(message);
    nullnet_set_input_callback(input_callback);

    
    memcpy(nullnet_buf, &message, sizeof(message));
    nullnet_len = sizeof(message);
    LOG_INFO("Sending %s\n", (char *)nullnet_buf);
    NETSTACK_NETWORK.output(NULL);

    // wait for a random time between 0 and 2 seconds
    etimer_set(&periodic_timer, rand() % (2 * CLOCK_SECOND));
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

    // if there is only one coordinator candidate, set it as parent
    if (coord_candidate_index == 1) {
        parent = coord_candidate[0];
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
        parent = coord_candidate[max_index];
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
        parent = sensor_candidate[max_index];
    }
    // if there is no coordinator candidate, set the edge node as parent
    else {
        parent = edge_node;
        type = 1;
        LOG_INFO("I am the coordinator\n");
        memcpy(nullnet_buf, "coordinator", sizeof("coordinator"));
        nullnet_len = sizeof("coordinator");
        NETSTACK_NETWORK.output(NULL);
    }

    LOG_INFO("Parent: %d.%d\n", parent.u8[0], parent.u8[1]);
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
    static struct etimer periodic_timer;
    static char message[20];

    PROCESS_BEGIN();

    /* Initialize NullNet */
    nullnet_buf = (uint8_t *)&message;
    nullnet_len = sizeof(message);
    nullnet_set_input_callback(input_callback);
    static int i;

    while (1){
        i=0;
        LOG_INFO("Sending poll message to children\n");
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
    static struct etimer periodic_timer;
    static char message[20];

    PROCESS_BEGIN();

    /* Initialize NullNet */
    nullnet_buf = (uint8_t *)&message;
    nullnet_len = sizeof(message);
    nullnet_set_input_callback(input_callback);
    while (1){
        etimer_set(&periodic_timer, 20 * CLOCK_SECOND);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
    }
    
    PROCESS_END();
}