// this is a cooja sensor node that will periodically send data to the coordinator node

#include "contiki.h"
#include "contiki-conf.h"
#include "net/rime.h"
#include "random.h"
#include "cc2420.h"
#include "cc2420_const.h"

// first the node must find the coordinator node:
// The nodes must organize themselves
// into a tree with the border router node as root of the tree. To select a parent node, a new sensor node:
// Checks if among the parent candidates, there is a coordinator node. If yes, the coordinator node become the parent.
// If there are multiple coordinator nodes as potential parent, the sensor nodes uses the signal strength to decide.
// If there are only sensor nodes as parent, the nodes uses the signal strength to decide.

static int max_candidate = 10;

static rimeaddr_t candidate_parents_coord[max_candidate];
int num_candidate_parents_coord = 0;
static rimeaddr_t candidate_parents_sensor[max_candidate];
int num_candidate_parents_sensor = 0;
static int signal_strength_coord[max_candidate];
static int signal_strength_sensor[max_candidate];
static rimeaddr_t parent;

static int type = 0; // 0 for sensor, 1 for coordinator

static void broadcast_recv(struct broadcast_conn *c, const rimeaddr_t *from)
{
    // add the sender to the list of candidate parents
    riemaddr_t sender = from;
    // check if the message is from a coordinator node
    if (strcmp(packetbuf_dataptr(), "coordinator") == 0)
    {
        printf("received coordinator message\n");
        if (num_candidate_parents_coord < max_candidate)
        {
        candidate_parents_coord[num_candidate_parents_coord] = sender;
        num_candidate_parents_coord++;
        signal_strength_coord[num_candidate_parents_coord] = cc2420_last_rssi;
        }
    }
    // if the message is from a sensor node, check if the sender is a coordinator node
    else if (strcmp(packetbuf_dataptr(), "sensor") == 0)
    {
        printf("received sensor message\n");
        if (num_candidate_parents_sensor < max_candidate)
        {
        candidate_parents_sensor[num_candidate_parents_sensor] = sender;
        num_candidate_parents_sensor++; 
        signal_strength_sensor[num_candidate_parents_sensor] = cc2420_last_rssi;
        }
    }
    // if the broadcast is a "new" message, send a response
    else if (strcmp(packetbuf_dataptr(), "new") == 0)
    {
        printf("received new message\n");
        // send a response to the sender
        packetbuf_copyfrom("sensor", 7);
        broadcast_send(&broadcast);
    }
    // if the message is "parent", set the type to coordinator
    else if (strcmp(packetbuf_dataptr(), "parent") == 0)
    {
        printf("received parent message\n");
        type = 1;
    }

}

// have a list of candidate parents addresses (rimeaddr_t)
static const struct broadcast_callbacks broadcast_call = {broadcast_recv};
static struct broadcast_conn broadcast;

static void recv_uc(struct unicast_conn *c, const rimeaddr_t *from)
{
    
}

static struct unicast_conn unicast;
static const struct unicast_callbacks unicast_callbacks = {recv_uc};

PROCESS(sensor_process, "Sensor process");
AUTOSTART_PROCESSES(&sensor_process);

PROCESS_THREAD(sensor_process, ev, data)
{   
    PROCESS_BEGIN();

    broadcast_open(&broadcast, 129, &broadcast_call);
    unicast_open(&unicast, 146, &unicast_callbacks);

    // broadcast message "new" & sensor_id 

    packetbuf_copyfrom("new", 4);
    broadcast_send(&broadcast);

    // wait for 2 seconds for responses
    static struct etimer et;
    etimer_set(&et, CLOCK_SECOND * 2);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    // check if there is only one candidate coordinator node, if yes set it as parent
    if (num_candidate_parents_coord == 1)
    {
        parent = candidate_parents_coord[0];
    }
    // if there are multiple coordinator nodes, choose the one with the highest signal strength
    else if (num_candidate_parents_coord > 1)
    {
        int max = signal_strength_coord[0];
        int max_index = 0;
        for (int i = 1; i < num_candidate_parents_coord; i++)
        {
            if (signal_strength_coord[i] > max)
            {
                max = signal_strength_coord[i];
                max_index = i;
            }
        }
        parent = candidate_parents_coord[max_index];
    }
    // if there are no coordinator nodes, choose the sensor node with the highest signal strength
    else if (num_candidate_parents_coord == 0)
    {
        int max = signal_strength_sensor[0];
        int max_index = 0;
        for (int i = 1; i < num_candidate_parents_sensor; i++)
        {
            if (signal_strength_sensor[i] > max)
            {
                max = signal_strength_sensor[i];
                max_index = i;
            }
        }
        parent = candidate_parents_sensor[max_index];
    }
    // if both are empty, set the parent to the border router
    else
    {
        parent.u8[0] = 0;
        parent.u8[1] = 0;
        type = 1;
    }

    // send a message to the parent node

    packetbuf_copyfrom("parent", 6);
    unicast_send(&unicast, &parent);
    
    PROCESS_END();
}