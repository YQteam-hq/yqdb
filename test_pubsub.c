#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "yq_pubsub.h"

static int message_count = 0;
static pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;

static int message_callback(yq_subscriber *sub, const yq_message *msg, void *user_data) {
    pthread_mutex_lock(&count_mutex);
    message_count++;
    pthread_mutex_unlock(&count_mutex);
    
    printf("Received message: ID=%lu, Topic=%.*s, Priority=%d, Size=%u bytes\n",
           msg->message_id, (int)msg->topic_len, msg->topic, 
           msg->priority, msg->payload_len);
    
    return 0; /* Success */
}

static void *publisher_thread(void *arg) {
    yq_pubsub *pubsub = (yq_pubsub *)arg;
    const char *topic = "test/topic";
    const char *payload = "Hello, World!";
    
    for (int i = 0; i < 10; i++) {
        uint64_t message_id;
        int result = yq_publish(pubsub, topic, payload, strlen(payload), 0, &message_id);
        
        if (result == 0) {
            printf("Published message %d with ID=%lu\n", i, message_id);
        } else {
            printf("Failed to publish message %d: %d\n", i, result);
        }
        
        usleep(100000); /* 100ms delay */
    }
    
    return NULL;
}

static void *subscriber_thread(void *arg) {
    yq_pubsub *pubsub = (yq_pubsub *)arg;
    
    int result = yq_subscribe(pubsub, "test/", NULL, message_callback, NULL, NULL);
    
    if (result == 0) {
        printf("Subscribed to test/ topics\n");
        
        /* Wait for messages */
        sleep(2);
        
        yq_subscriber *sub = NULL; /* This would be returned by yq_subscribe */
        result = yq_unsubscribe(sub);
        if (result == 0) {
            printf("Unsubscribed from test/ topics\n");
        }
    } else {
        printf("Failed to subscribe: %d\n", result);
    }
    
    return NULL;
}

int main() {
    printf("Testing yq-DB Pub/Sub functionality...\n");
    
    /* Create pub/sub instance */
    yq_pubsub *pubsub = yq_pubsub_create(1024, 1000, 5000);
    if (!pubsub) {
        printf("Failed to create pubsub instance\n");
        return 1;
    }
    
    printf("Pub/Sub instance created successfully\n");
    
    /* Test basic operations */
    yq_pubsub_stats stats;
    int result = yq_pubsub_get_stats(pubsub, &stats);
    
    if (result == 0) {
        printf("Pub/Sub stats: Messages=%lu, Bytes=%lu\n", 
               stats.total_messages, stats.total_bytes);
    }
    
    /* Test topic operations */
    const char *topic = "test/example";
    result = yq_topic_exists(pubsub, topic);
    
    if (result == 0) {
        printf("Topic '%s' exists\n", topic);
    } else if (result == YQ_ERR_NOTFOUND) {
        printf("Topic '%s' does not exist\n", topic);
    }
    
    /* Test publishing */
    uint64_t message_id;
    const char *payload = "Test message for pub/sub";
    result = yq_publish(pubsub, topic, payload, strlen(payload), 0, &message_id);
    
    if (result == 0) {
        printf("Published message with ID=%lu\n", message_id);
    } else {
        printf("Failed to publish: %d\n", result);
        return 1;
    }
    
    /* Test subscription */
    result = yq_subscribe(pubsub, "test/", NULL, message_callback, NULL, NULL);
    
    if (result == 0) {
        printf("Subscribed to test/ topics\n");
    } else {
        printf("Failed to subscribe: %d\n", result);
        return 1;
    }
    
    /* Test multi-threading */
    pthread_t pub_thread, sub_thread;
    
    result = pthread_create(&pub_thread, NULL, publisher_thread, pubsub);
    if (result != 0) {
        printf("Failed to create publisher thread\n");
        return 1;
    }
    
    result = pthread_create(&sub_thread, NULL, subscriber_thread, pubsub);
    if (result != 0) {
        printf("Failed to create subscriber thread\n");
        return 1;
    }
    
    /* Wait for threads */
    pthread_join(pub_thread, NULL);
    pthread_join(sub_thread, NULL);
    
    /* Check final message count */
    printf("Total messages received: %d\n", message_count);
    
    /* Clean up */
    yq_pubsub_destroy(pubsub);
    printf("Pub/Sub test completed successfully\n");
    
    return 0;
}