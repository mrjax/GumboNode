#include "gumbo_utils.h"
#include "net/packetbuf.h"
#include "sys/etimer.h"
#include "sys/ctimer.h"
#include "sys/node-id.h"
#include "lib/random.h"

/* master states */
#define OFF   0
#define ON    1

/* slave states */
#define WAITING_FOR_CONTINUE    1
#define WAITING_FOR_CONFIRM     2
#define WAITING_FOR_QUERY       3
#define WAITING_FOR_DATA        4
#define SENDING_DATABASE        5

PROCESS(gumbo_master, "Top-level process for a Gumbo node.");
PROCESS(gumbo_slave, "Sending and receiving management process.");
AUTOSTART_PROCESSES(&gumbo_master);

struct slave_packet_info {
  gumbo_opcode_t opcode;
  gumbo_addr_t addr;
};

extern const struct radio_driver cooja_radio_driver;
static gumbo_addr_t node_address;

void receive_handler(const char *, int);
void send_query_handler(void *);
void dump_opcode_info(struct slave_packet_info *);

PROCESS_THREAD(gumbo_master, ev, data)
{
  PROCESS_BEGIN();
  
  static struct etimer on_off_timer;
  static int state = OFF;
  
  clock_time_t interval;

  node_address = (gumbo_addr_t) simMoteID;

  list_init(gumbo_node_entries);
  
  interval = (random_rand() % 5 + 20) * CLOCK_SECOND;
  etimer_set(&on_off_timer, interval);
  init_receive_callback(&receive_handler);
  cooja_radio_driver.init();  
        
  while (1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&on_off_timer));
    interval = (random_rand() % 5 + 20) * CLOCK_SECOND;
    etimer_set(&on_off_timer, interval);

    switch (state) {
    case OFF:
      cooja_radio_driver.on();
      process_start(&gumbo_slave, NULL);
    
      state = ON;
      break;
    
    case ON:
      cooja_radio_driver.off();
      process_exit(&gumbo_slave);
      
      state = OFF;
      break;
      
    default:
      etimer_reset(&on_off_timer);
      state = OFF;
    }
  }
  
  PROCESS_END();
}

PROCESS_THREAD(gumbo_slave, ev, data)
{
  PROCESS_BEGIN();
  
  static int state = WAITING_FOR_QUERY;
  struct slave_packet_info *pinfo;
  
  static gumbo_addr_t sender_address;
  
  char buffer[MESSAGE_SIZE+1];
  int pstatus;
  
  static struct ctimer send_timer;
  clock_time_t interval = (random_rand() % 5 + 1) * CLOCK_SECOND;
  ctimer_set(&send_timer, interval, send_query_handler, &node_address);
  
  while (1) {
    PROCESS_WAIT_EVENT_UNTIL(ev == PROCESS_EVENT_MSG);
    pinfo = (struct slave_packet_info *) data;
    
    if (pinfo->opcode == QUERY_OPCODE) {
      send_confirm_message(node_address);
      state = WAITING_FOR_CONTINUE;
    } else {
      switch (state) {
      case WAITING_FOR_CONTINUE:
        if (pinfo->opcode == CONTINUE_OPCODE && pinfo->addr == node_address) {
          send_first_data_packet();
          printf("Sending data packets.\n");
          state = SENDING_DATABASE;
        }
        
        break;
      
      case SENDING_DATABASE:
        if (pinfo->addr == node_address) {
          if (pinfo->opcode == CONTINUE_OPCODE) {
            pstatus = send_next_data_packet();
            if (pstatus == END_OF_DATA)
              state = WAITING_FOR_QUERY;
          }
          else if (pinfo->opcode == STOP_OPCODE)
            state = WAITING_FOR_QUERY;
        }
        
        break;
      
      case WAITING_FOR_CONFIRM:
        if (pinfo->opcode == CONFIRM_OPCODE) {
          sender_address = pinfo->addr;
          send_continue_message(sender_address);
          state = WAITING_FOR_DATA;
        }
        
        break;
        
      case WAITING_FOR_DATA:
        if (pinfo->opcode == NEW_DATA)
          send_continue_message(sender_address);
        else {
          send_stop_message(sender_address);
          state = WAITING_FOR_QUERY;
        }
        
        break;
        
      case WAITING_FOR_QUERY:
        break;
      
      default:
        state = WAITING_FOR_QUERY;
      }
    }
  }
  
  PROCESS_END();
}

void receive_handler(const char *msg, int len) {
  static struct slave_packet_info info;

  if (len > MESSAGE_SIZE) {
    printf("Message in buffer too large to copy.\n");
    return;
  }
  
  if (is_opcode_packet(msg)) {
    info.opcode = OPCODE(msg);
    info.addr   = WIDE_DATA(msg);
    dump_opcode_info(&info);
    process_post(&gumbo_slave, PROCESS_EVENT_MSG, &info);
  }
  
  else {
    info.opcode = log_and_save(msg);
    info.addr   = 0;
    
    if (info.opcode == NEW_DATA)
      printf("New node data received.\n");
    else if (info.opcode == OLD_DATA)
      printf("Old node data received.\n");
    
    if (info.opcode == NEW_DATA || info.opcode == OLD_DATA)
      process_post(&gumbo_slave, PROCESS_EVENT_MSG, &info);
  }
}

void send_query_handler(void *arg) {
  gumbo_addr_t *ptr_addr = (gumbo_addr_t *) arg;
  send_query_message(*ptr_addr);
}

void dump_opcode_info(struct slave_packet_info *info) {
  switch (info->opcode) {
  case QUERY_OPCODE:
    printf("Query packet received (addr: %d).\n", info->addr);
    break;
  case BEGIN_OPCODE:
    printf("Begin packet received (addr: %d).\n", info->addr);
    break;
  case CONTINUE_OPCODE:
    printf("Confirm/Continue packet received (addr: %d).\n", info->addr);
    break;
  case STOP_OPCODE:
    printf("Stop packet received (addr: %d).\n", info->addr);
    break;
  default:
    printf("Unrecognized packet received (addr: %d).\n", info->addr);
  }
}

