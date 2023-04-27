#include "contiki.h"
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
#define MAX_CANDIDATE 10
/* Configuration */
#define SEND_INTERVAL (8 * CLOCK_SECOND)


/*---------------------------------------------------------------------------*/
PROCESS(nullnet_example_process, "NullNet broadcast example");
AUTOSTART_PROCESSES(&nullnet_example_process);

static linkaddr_t edge_node = { { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } };
static linkaddr_t coord_candidate[MAX_CANDIDATE];
static int coord_candidate_index = 0;
static linkaddr_t sensor_candidate[MAX_CANDIDATE];
static int sensor_candidate_index = 0;
static linkaddr_t parent;
static int coord_candidate_rssi[MAX_CANDIDATE];
static int sensor_candidate_rssi[MAX_CANDIDATE];
static int type = 0; // 0: sensor, 1: coordinator

/*---------------------------------------------------------------------------*/
void input_callback(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest)
{
    static char message[20];
    memcpy(&message, data, len);
    // if message is "coordinator", add src to coord_candidate
    if (strcmp(message, "coordinator") == 0) {
        memcpy(&coord_candidate[coord_candidate_index], src, sizeof(linkaddr_t));
        coord_candidate_rssi[coord_candidate_index] = cc2420_last_rssi;
        coord_candidate_index++;
        
    }
    // if message is "sensor", add src to sensor_candidate
    else if (strcmp(message, "sensor") == 0) {
        memcpy(&sensor_candidate[sensor_candidate_index], src, sizeof(linkaddr_t));
        sensor_candidate_rssi[sensor_candidate_index] = cc2420_last_rssi;
        sensor_candidate_index++;
    }
    // if message is "child", we are coordinator
    else if (strcmp(message, "child") == 0) {
        type = 1;
    }
    // if message is new, send our type
    else if (strcmp(message, "new") == 0) {
        if (type == 0) {
            memcpy(nullnet_buf, "sensor", sizeof("sensor"));
            nullnet_len = sizeof("sensor");
            NETSTACK_NETWORK.output(NULL);
        }
        else {
            memcpy(nullnet_buf, "coordinator", sizeof("coordinator"));
            nullnet_len = sizeof("coordinator");
            NETSTACK_NETWORK.output(NULL);
        }
    }

}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(nullnet_example_process, ev, data)
{
    static struct etimer periodic_timer;
    static char message[20] = "new";

    PROCESS_BEGIN();

    /* Initialize NullNet */
    nullnet_buf = (uint8_t *)&message;
    nullnet_len = sizeof(message);
    nullnet_set_input_callback(input_callback);

    
    memcpy(nullnet_buf, &message, sizeof(message));
    nullnet_len = sizeof(message);
    LOG_INFO("Sending %s\n", (char *)nullnet_buf);
    NETSTACK_NETWORK.output(NULL);

    // wait for 2 seconds
    etimer_set(&periodic_timer, 2 * CLOCK_SECOND);
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
    }

    // send child to parent
    memcpy(nullnet_buf, "child", sizeof("child"));
    nullnet_len = sizeof("child");
    NETSTACK_NETWORK.output(&parent);
    while (1){
        // wait for 5 seconds
        etimer_set(&periodic_timer, 5 * CLOCK_SECOND);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
        LOG_INFO("type %d\n", type);
    }

  PROCESS_END();
}

