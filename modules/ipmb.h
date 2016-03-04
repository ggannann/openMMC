/*
 *   openMMC -- Open Source modular IPM Controller firmware
 *
 *   Copyright (C) 2015-2016  Henrique Silva <henrique.silva@lnls.br>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *   @license GPL-3.0+ <http://spdx.org/licenses/GPL-3.0+>
 */

/*!
 * @file ipmb.h
 * @author Henrique Silva <henrique.silva@lnls.br>, LNLS
 * @date August 2015
 *
 * @brief Definitions used in IPMB Layer
 */

#ifndef IPMB_H_
#define IPMB_H_

/* FreeRTOS includes */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/*! @brief Default I2C interface to use in IPMB protocol */
#define IPMB_I2C                I2C0

/*! @brief Maximum count of messages to be sent */
#define IPMB_TXQUEUE_LEN        5
/*! @brief Maximum count if received messages to be delivered to client task  */
#define IPMB_CLIENT_QUEUE_LEN   5

/*! @brief Maximum retries made by IPMB TX Task when sending a message */
#define IPMB_MAX_RETRIES        3

/*! @brief Timeout limit between the end of a request and start of a response (defined in IPMB timing specifications)
 */
#define IPMB_MSG_TIMEOUT        250/portTICK_PERIOD_MS

/*! @brief Timeout limit waiting a free space in client queue to put a received message
 */
#define CLIENT_NOTIFY_TIMEOUT   5

/*! @brief Position of Header cheksum byte in a IPMI message
 */
#define IPMI_HEADER_CHECKSUM_POSITION      2
/*! @brief Maximum length in bytes of a IPMI message (including connection header)
 */
#define IPMI_MSG_MAX_LENGTH         32
/*! @brief Length of connection header in a request
 *
 * The connection header in a request is made by:
 * - rsSA -> Destination Address
 * - NetFN:6 rsLUN:2 -> Net function and dest LUN
 * - HeaderChecksum -> 2's complement of the sum of preceding bytes
 * - rqSA -> Source address
 * - rqSeq:6 rqLUN:2 -> Sequence Number and source LUN
 * - CMD -> Command
 * - Messagechecksum -> 2's complement of the sum of preceding bytes
 */
#define IPMB_REQ_HEADER_LENGTH      6
/* Response header is 1 byte longer because it must include the completion code */

/*! @brief Length of connection header in a response
 *
 * The connection header in a response is made by:
 * - rqSA -> Destination Address
 * - NetFN:6 rqLUN:2 -> Net function and dest LUN
 * - HeaderChecksum -> 2's complement of the sum of preceding bytes
 * - rsSA -> Source address
 * - rqSeq:6 rsLUN:2 -> Sequence Number and source LUN
 * - CMD -> Command
 * - CC -> Completion Code
 * - Messagechecksum -> 2's complement of the sum of preceding bytes
 */
#define IPMB_RESP_HEADER_LENGTH     7

#define IPMB_NETFN_MASK         0xFC
#define IPMB_DEST_LUN_MASK      0x3
#define IPMB_SEQ_MASK           0xFC
#define IPMB_SRC_LUN_MASK       0x3

/*! @brief MicroTCA's MCH slave address */
#define MCH_ADDRESS             0x20

/* @brief Macro to check is the message is a response (odd netfn) */
#define IS_RESPONSE(msg) (msg.netfn & 0x01)

/*! @brief Size of #IPMBL_TABLE
 */
#define IPMBL_TABLE_SIZE                27

/*! @brief GA pins definition */
typedef enum {
    GROUNDED = 0,
    POWERED,
    UNCONNECTED
}GA_Pin_state;

/*! @brief IPMI message struct */
typedef struct ipmi_msg {
    uint8_t dest_addr;                  /*!< Destination slave address */
    uint8_t netfn;                      /*!< Net Function
                                         * @see ipmi.h
                                         */
    uint8_t dest_LUN;                   /*!< Destination LUN (Logical Unit Number) */
    uint8_t hdr_chksum;                 /*!< Connection Header Checksum */
    uint8_t src_addr;                   /*!< Source slave address */
    uint8_t seq;                        /*!< Sequence Number */
    uint8_t src_LUN;                    /*!< Source LUN (Logical Unit Number) */
    uint8_t cmd;                        /*!< Command
                                         * @see ipmi.h
                                         */
    uint8_t completion_code;            /*!< Completion Code
                                         * @see ipmi.h
                                         */
    uint8_t data_len;                   /*!< Amount of valid bytes in #data buffer */
    uint8_t data[IPMI_MSG_MAX_LENGTH];  /*!< Data buffer <br>
                                         * Data field has 24 bytes:
                                         * 32 (Max IPMI msg len) - 7 header bytes - 1 final chksum byte
                                         */
    uint8_t msg_chksum;                 /*!< Message checksum */
} ipmi_msg;

typedef struct ipmi_msg_cfg {
    ipmi_msg buffer;
    TaskHandle_t caller_task;
    uint8_t retries;
    uint32_t timestamp;
} ipmi_msg_cfg;

/*! @brief IPMB errors enumeration */
typedef enum ipmb_error {
    ipmb_error_unknown = 0,
    ipmb_error_success,                 /*!< Generic no-error flag  */
    ipmb_error_failure,                 /*!< Generic failure on IPMB */
    ipmb_error_timeout,                 /*!< Error raised when a message takes too long to be responded */
    ipmb_error_invalid_req,             /*!< A invalid request was received */
    ipmb_error_hdr_chksum,              /*!< Invalid header checksum from incoming message */
    ipmb_error_msg_chksum,              /*!< Invalid message checksum from incoming message */
    ipmb_error_queue_creation           /*!< Client queue couldn't be created. Invalid pointer to handler was given */
} ipmb_error;

extern uint8_t ipmb_addr;

/* Function Prototypes */

/*! @brief IPMB Transmitter Task
 *
 * When #ipmb_send_request or #ipmb_send_response put a message in #ipmb_txqueue, this task unblocks.
 * First step to send a message is differentiating requests from responses. It does this analyzing the parity of NetFN (even for requests, odd for responses).
 *
 * When sending a response, this task has to check if it matches with the sequence number from the last known request and the amount of time it took to be built (timeout checking). Last step is checking if it has already tried to send this message more than #IPMB_MAX_RETRIES value. <br>
 * After passing all checking, the message is formatted as the IPMB protocol demands and passed down to the I2C driver, using the function xI2CWrite(). <br>
 * If an error comes out of the I2C driver when sending the message, it increases the retry counter in the #ipmi_msg_cfg struct and send the message to the front of #ipmb_txqueue. <br>
 * If no errors occurs, the task that put the message in the queue is notified with a success flag.
 *
 * The proccess is analog when sending a request, but the only check that is made is the retry number. The task skip all checking because, when sending a request, the message is formatted using it's own functions #ipmb_send_request or #ipmb_send_response and they are guaranteed to put only valid messages in queue.
 * @param pvParameters: Default parameter to FreeRTOS tasks, not used here.
 * @see IPMB_RXTask
 * @see ipmb_send_request
 * @see ipmb_send_response
 */
void IPMB_TXTask ( void *pvParameters );

/*! @brief IPMB Receiver Task
 *
 * Similarly to #IPMB_TXTask, this task remains blocked until a new message is received by the I2C driver. The message passes through checksum checking to assure its integrity. <br>
 * If the message is a request, we have to check if it's a new one or just a retransmission of the last. In order to do this, the sequential number is tested, since every request has a different one.<br>
 * Right after that, the arrival time and the message body are stored for future checking and the specified client is notified using #ipmb_notify_client.
 *
 * If we have received a response instead, we match it with the last stored request and also check if the awating request hasn't timed-out yet.
 *
 * @note When a malformed message, a response without a request or a repeated request are received, they are just ignored, following the IPMB specifications.
 *
 * @param pvParameters: Default parameter to FreeRTOS tasks, not used here.
 * @see IPMB_TXTask
 * @see ipmb_notify_client
 */
void IPMB_RXTask ( void *pvParameters );

/*! @brief Initializes the IPMB Layer.
 *
 * Configures the I2C Driver, creates the TX queue for the IPMB Task and both IPMB RX and IPMB TX tasks
 */
void ipmb_init ( void );

/*! @brief Format and send a request via IPMB channel
 */
ipmb_error ipmb_send_request ( ipmi_msg * req );
ipmb_error ipmb_send_response ( ipmi_msg * req, ipmi_msg * resp );

/*! @brief Creates and returns a queue in which the client can block to receive the incoming requests.
 *
 * The queue is created and its handler is written at the given pointer (queue).
 * Also keeps a copy of the handler to know where to write the incoming messages.
 *
 * @param queue Pointer to a QueueHandle_t variable which will be written by this function.
 *
 * @retval ipmb_error_success The queue was successfully created.
 * @retval ipmb_error_queue_creation Queue creation failed due to lack of Heap space.
 */
ipmb_error ipmb_register_rxqueue ( QueueHandle_t * queue );

ipmb_error ipmb_assert_chksum ( uint8_t * buffer, uint8_t buffer_len );

/*! @brief Reads own I2C slave address using GA pins
 *
 * Based on coreipm/coreipm/mmc.c
 * @author Gokhan Sozmen
 * Reads the GA pins, performing an unconnection checking, to define the device I2C slave address, as specified by MicroTCA documentation.
 *
 * @return 7-bit Slave Address
 *
 * @todo Develop a function to discover the Geographic Address once (checking the GA pins)
 * and store it into a global variable, since everytime a IPMI message is built
 * (request or response) the MMC has to check its own  address to fill the rs/rqSA field,
 * and it takes some time to go through all this function.
 */
uint8_t get_ipmb_addr( void );

#endif
