#include "ua_services.h"
#include "ua_server_internal.h"
#include "ua_subscription_manager.h"
#include "ua_subscription.h"
#include "ua_statuscodes.h"
#include "ua_util.h"
#include "ua_nodestore.h"

#define UA_BOUNDEDVALUE_SETWBOUNDS(BOUNDS, SRC, DST) { \
    if(SRC > BOUNDS.maxValue) DST = BOUNDS.maxValue; \
    else if(SRC < BOUNDS.minValue) DST = BOUNDS.minValue; \
    else DST = SRC; \
    }

void Service_CreateSubscription(UA_Server *server, UA_Session *session,
                                const UA_CreateSubscriptionRequest *request,
                                UA_CreateSubscriptionResponse *response) {
    response->subscriptionId = SubscriptionManager_getUniqueUIntID(&session->subscriptionManager);
    UA_Subscription *newSubscription = UA_Subscription_new(response->subscriptionId);
    
    UA_BOUNDEDVALUE_SETWBOUNDS(session->subscriptionManager.GlobalPublishingInterval,
                               request->requestedPublishingInterval, response->revisedPublishingInterval);
    newSubscription->PublishingInterval = response->revisedPublishingInterval;
    
    UA_BOUNDEDVALUE_SETWBOUNDS(session->subscriptionManager.GlobalLifeTimeCount,
                               request->requestedLifetimeCount, response->revisedLifetimeCount);
    newSubscription->LifeTime = (UA_UInt32_BoundedValue)  {
        .minValue=session->subscriptionManager.GlobalLifeTimeCount.minValue,
        .maxValue=session->subscriptionManager.GlobalLifeTimeCount.maxValue,
        .currentValue=response->revisedLifetimeCount};
    
    UA_BOUNDEDVALUE_SETWBOUNDS(session->subscriptionManager.GlobalKeepAliveCount,
                               request->requestedMaxKeepAliveCount, response->revisedMaxKeepAliveCount);
    newSubscription->KeepAliveCount = (UA_Int32_BoundedValue)  {
        .minValue=session->subscriptionManager.GlobalKeepAliveCount.minValue,
        .maxValue=session->subscriptionManager.GlobalKeepAliveCount.maxValue,
        .currentValue=response->revisedMaxKeepAliveCount};
    
    newSubscription->NotificationsPerPublish = request->maxNotificationsPerPublish;
    newSubscription->PublishingMode          = request->publishingEnabled;
    newSubscription->Priority                = request->priority;
    
    UA_Guid jobId = SubscriptionManager_getUniqueGUID(&session->subscriptionManager);
    Subscription_createdUpdateJob(server, jobId, newSubscription);
    Subscription_registerUpdateJob(server, newSubscription);
    SubscriptionManager_addSubscription(&session->subscriptionManager, newSubscription);    
}

static void createMonitoredItems(UA_Server *server, UA_Session *session,
                                 UA_Subscription *sub, const UA_MonitoredItemCreateRequest *request,
                                 UA_MonitoredItemCreateResult *result) {
    const UA_Node *target = UA_NodeStore_get(server->nodestore, &request->itemToMonitor.nodeId);
    if(!target) {
        result->statusCode = UA_STATUSCODE_BADNODEIDINVALID;
        result->monitoredItemId = 0;
        result->revisedSamplingInterval = 0;
        result->revisedQueueSize = 0;
        return;
    }

    UA_MonitoredItem *newMon = UA_MonitoredItem_new();
    UA_NodeId_copy(&target->nodeId, &newMon->monitoredNodeId);
    newMon->ItemId = ++(session->subscriptionManager.LastSessionID);
    result->monitoredItemId = newMon->ItemId;
    newMon->ClientHandle = request->requestedParameters.clientHandle;
    UA_BOUNDEDVALUE_SETWBOUNDS(session->subscriptionManager.GlobalSamplingInterval,
                               request->requestedParameters.samplingInterval,
                               result->revisedSamplingInterval);
    newMon->SamplingInterval = result->revisedSamplingInterval;
    UA_BOUNDEDVALUE_SETWBOUNDS(session->subscriptionManager.GlobalQueueSize,
                               request->requestedParameters.queueSize,
                               result->revisedQueueSize);
    newMon->QueueSize = (UA_UInt32_BoundedValue) {
        .maxValue=(result->revisedQueueSize) + 1,
        .minValue=0, .currentValue=0 };
    newMon->AttributeID = request->itemToMonitor.attributeId;
    newMon->MonitoredItemType = MONITOREDITEM_TYPE_CHANGENOTIFY;
    newMon->DiscardOldest = request->requestedParameters.discardOldest;
    LIST_INSERT_HEAD(&sub->MonitoredItems, newMon, listEntry);
    UA_NodeStore_release(target);
}

void Service_CreateMonitoredItems(UA_Server *server, UA_Session *session,
                                  const UA_CreateMonitoredItemsRequest *request,
                                  UA_CreateMonitoredItemsResponse *response) {
    UA_Subscription  *sub = SubscriptionManager_getSubscriptionByID(&session->subscriptionManager,
                                                                    request->subscriptionId);
    if(!sub) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;
        return;
    }
    
    if(request->itemsToCreateSize <= 0) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADNOTHINGTODO;
        return;
    }

    response->results = UA_Array_new(&UA_TYPES[UA_TYPES_MONITOREDITEMCREATERESULT], request->itemsToCreateSize);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }
    response->resultsSize = request->itemsToCreateSize;

    for(UA_Int32 i = 0; i<request->itemsToCreateSize; i++)
        createMonitoredItems(server, session, sub, &request->itemsToCreate[i], &response->results[i]);
}

static void publish(UA_Server *server, UA_Session *session, UA_Subscription *sub,
                    const UA_PublishRequest *request, UA_PublishResponse *response) {

}

void Service_Publish(UA_Server *server, UA_Session *session, const UA_PublishRequest *request,
                     UA_PublishResponse *response) {
    UA_SubscriptionManager *manager= &session->subscriptionManager;
    if(!manager)
        return;
    
    // Delete Acknowledged Subscription Messages
    response->resultsSize = request->subscriptionAcknowledgementsSize;
    response->results     = UA_malloc(sizeof(UA_StatusCode)*(response->resultsSize));
    for(UA_Int32 i = 0; i < request->subscriptionAcknowledgementsSize; i++) {
        response->results[i] = UA_STATUSCODE_GOOD;
        UA_Subscription *sub =
            SubscriptionManager_getSubscriptionByID(&session->subscriptionManager,
                                                    request->subscriptionAcknowledgements[i].subscriptionId);
        if(!sub) {
            response->results[i] = UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;
            continue;
        }
        if(Subscription_deleteUnpublishedNotification(request->subscriptionAcknowledgements[i].sequenceNumber,
                                                      sub) == 0)
            response->results[i] = UA_STATUSCODE_BADSEQUENCENUMBERINVALID;
    }
    
    // See if any new data is available
    UA_Subscription *sub;
    LIST_FOREACH(sub, &manager->ServerSubscriptions, listEntry) {
        if(sub->timedUpdateIsRegistered == UA_FALSE) {
            // FIXME: We are forcing a value update for monitored items. This should be done by the event system.
            // NOTE:  There is a clone of this functionality in the Subscription_timedUpdateNotificationsJob
            UA_MonitoredItem *mon;
            LIST_FOREACH(mon, &sub->MonitoredItems, listEntry)
                MonitoredItem_QueuePushDataValue(server, mon);
            
            // FIXME: We are forcing notification updates for the subscription. This
            // should be done by a timed work item.
            Subscription_updateNotifications(sub);
        }
        
        if(Subscription_queuedNotifications(sub) <= 0)
            continue;
        
        response->subscriptionId = sub->SubscriptionID;
        Subscription_copyTopNotificationMessage(&response->notificationMessage, sub);
        if(sub->unpublishedNotifications.lh_first->notification->sequenceNumber > sub->SequenceNumber) {
            // If this is a keepalive message, its seqNo is the next seqNo to be used for an actual msg.
            response->availableSequenceNumbersSize = 0;
            // .. and must be deleted
            Subscription_deleteUnpublishedNotification(sub->SequenceNumber + 1, sub);
        } else {
            response->availableSequenceNumbersSize = Subscription_queuedNotifications(sub);
            response->availableSequenceNumbers = Subscription_getAvailableSequenceNumbers(sub);
        }	  
        // FIXME: This should be in processMSG();
        session->validTill = UA_DateTime_now() + session->timeout * 10000;
        return;
    }
    
    // FIXME: At this point, we would return nothing and "queue" the publish
    // request, but currently we need to return something to the client. If no
    // subscriptions have notifications, force one to generate a keepalive so we
    // don't return an empty message
    sub = LIST_FIRST(&manager->ServerSubscriptions);
    if(!sub) {
        response->subscriptionId = sub->SubscriptionID;
        sub->KeepAliveCount.currentValue=sub->KeepAliveCount.minValue;
        Subscription_generateKeepAlive(sub);
        Subscription_copyTopNotificationMessage(&response->notificationMessage, sub);
        Subscription_deleteUnpublishedNotification(sub->SequenceNumber + 1, sub);
    }
    
    // FIXME: This should be in processMSG();
    session->validTill = UA_DateTime_now() + session->timeout * 10000;
}

void Service_ModifySubscription(UA_Server *server, UA_Session *session,
                                 const UA_ModifySubscriptionRequest *request,
                                 UA_ModifySubscriptionResponse *response) {
    UA_Subscription *sub = SubscriptionManager_getSubscriptionByID(&session->subscriptionManager,
                                                                   request->subscriptionId);
    if(!sub) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;
        return;
    }
    
    UA_BOUNDEDVALUE_SETWBOUNDS(session->subscriptionManager.GlobalPublishingInterval,
                               request->requestedPublishingInterval, response->revisedPublishingInterval);
    sub->PublishingInterval = response->revisedPublishingInterval;
    
    UA_BOUNDEDVALUE_SETWBOUNDS(session->subscriptionManager.GlobalLifeTimeCount,
                               request->requestedLifetimeCount, response->revisedLifetimeCount);
    sub->LifeTime = (UA_UInt32_BoundedValue)  {
        .minValue=session->subscriptionManager.GlobalLifeTimeCount.minValue,
        .maxValue=session->subscriptionManager.GlobalLifeTimeCount.maxValue,
        .currentValue=response->revisedLifetimeCount};
        
    UA_BOUNDEDVALUE_SETWBOUNDS(session->subscriptionManager.GlobalKeepAliveCount,
                                request->requestedMaxKeepAliveCount, response->revisedMaxKeepAliveCount);
    sub->KeepAliveCount = (UA_Int32_BoundedValue)  {
        .minValue=session->subscriptionManager.GlobalKeepAliveCount.minValue,
        .maxValue=session->subscriptionManager.GlobalKeepAliveCount.maxValue,
        .currentValue=response->revisedMaxKeepAliveCount};
        
    sub->NotificationsPerPublish = request->maxNotificationsPerPublish;
    sub->Priority                = request->priority;
    
    Subscription_registerUpdateJob(server, sub);
    return;
}

void Service_DeleteSubscriptions(UA_Server *server, UA_Session *session,
                                 const UA_DeleteSubscriptionsRequest *request,
                                 UA_DeleteSubscriptionsResponse *response) {
    response->results = UA_malloc(sizeof(UA_StatusCode) * request->subscriptionIdsSize);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }
    response->resultsSize = request->subscriptionIdsSize;

    for(UA_Int32 i = 0; i < request->subscriptionIdsSize; i++)
        response->results[i] = SubscriptionManager_deleteSubscription(server, &session->subscriptionManager,
                                                                      request->subscriptionIds[i]);
} 

void Service_DeleteMonitoredItems(UA_Server *server, UA_Session *session,
                                  const UA_DeleteMonitoredItemsRequest *request,
                                  UA_DeleteMonitoredItemsResponse *response) {
    UA_SubscriptionManager *manager = &session->subscriptionManager;
    UA_Subscription *sub = SubscriptionManager_getSubscriptionByID(manager, request->subscriptionId);
    if(!sub) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;
        return;
    }
    
    response->results = UA_malloc(sizeof(UA_StatusCode) * request->monitoredItemIdsSize);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }
    response->resultsSize = request->monitoredItemIdsSize;

    for(UA_Int32 i = 0; i < request->monitoredItemIdsSize; i++)
        response->results[i] = SubscriptionManager_deleteMonitoredItem(manager, sub->SubscriptionID,
                                                                       request->monitoredItemIds[i]);
}