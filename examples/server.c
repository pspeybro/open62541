/*
 * This work is licensed under a Creative Commons CCZero 1.0 Universal License.
 * See http://creativecommons.org/publicdomain/zero/1.0/ for more information.
 */

#include <signal.h>
#include <errno.h> // errno, EINTR
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> //access

#define __USE_XOPEN2K
#ifdef UA_MULTITHREADING
# include <pthread.h>
#endif

#ifdef NOT_AMALGATED
# include <time.h>
# include "ua_types.h"
# include "ua_server.h"
# include "logger_stdout.h"
# include "networklayer_tcp.h"
#else
# include "open62541.h"
#endif

/****************************/
/* Server-related variables */
/****************************/

UA_Boolean running = 1;
UA_Logger logger;

/*************************/
/* Read-only data source */
/*************************/
static UA_StatusCode readTimeData(void *handle, UA_Boolean sourceTimeStamp,
                                  const UA_NumericRange *range, UA_DataValue *value) {
    if(range) {
        value->hasStatus = UA_TRUE;
        value->status = UA_STATUSCODE_BADINDEXRANGEINVALID;
        return UA_STATUSCODE_GOOD;
    }
	UA_DateTime *currentTime = UA_DateTime_new();
	if(!currentTime)
		return UA_STATUSCODE_BADOUTOFMEMORY;
	*currentTime = UA_DateTime_now();
	value->value.type = &UA_TYPES[UA_TYPES_DATETIME];
	value->value.arrayLength = -1;
	value->value.data = currentTime;
	value->value.arrayDimensionsSize = -1;
	value->value.arrayDimensions = NULL;
	value->hasValue = UA_TRUE;
	if(sourceTimeStamp) {
		value->hasSourceTimestamp = UA_TRUE;
		value->sourceTimestamp = *currentTime;
	}
	return UA_STATUSCODE_GOOD;
}

static void releaseTimeData(void *handle, UA_DataValue *value) {
    if(value->hasValue)
        UA_DateTime_delete((UA_DateTime*)value->value.data);
}

/*****************************/
/* Read-only CPU temperature */
/*      Only on Linux        */
/*****************************/
FILE* temperatureFile = NULL;
static UA_StatusCode readTemperature(void *handle, UA_Boolean sourceTimeStamp,
                                     const UA_NumericRange *range, UA_DataValue *value) {
    if(range) {
        value->hasStatus = UA_TRUE;
        value->status = UA_STATUSCODE_BADINDEXRANGEINVALID;
        return UA_STATUSCODE_GOOD;
    }

	UA_Double* currentTemperature = UA_Double_new();

	if(!currentTemperature)
		return UA_STATUSCODE_BADOUTOFMEMORY;

	fseek(temperatureFile, 0, SEEK_SET);

	if(fscanf(temperatureFile, "%lf", currentTemperature) != 1){
		UA_LOG_WARNING(logger, UA_LOGCATEGORY_USERLAND, "Can not parse temperature");
		exit(1);
	}

	*currentTemperature /= 1000.0;

	value->value.type = &UA_TYPES[UA_TYPES_DOUBLE];
	value->value.arrayLength = -1;
	value->value.data = currentTemperature;
	value->value.arrayDimensionsSize = -1;
	value->value.arrayDimensions = NULL;
	value->hasValue = UA_TRUE;
	return UA_STATUSCODE_GOOD;
}

static void releaseTemperature(void *handle, UA_DataValue *value) {
    if(value->hasValue)
        UA_Double_delete((UA_Double*)value->value.data);
}

/*************************/
/* Read-write status led */
/*************************/
#ifdef UA_MULTITHREADING
pthread_rwlock_t writeLock;
#endif
FILE* triggerFile = NULL;
FILE* ledFile = NULL;
UA_Boolean ledStatus = 0;

static UA_StatusCode readLedStatus(void *handle, UA_Boolean sourceTimeStamp,
                                   const UA_NumericRange *range, UA_DataValue *value) {
    if(range) {
        value->hasStatus = UA_TRUE;
        value->status = UA_STATUSCODE_BADINDEXRANGEINVALID;
        return UA_STATUSCODE_GOOD;
    }

	/* In order to reduce blocking time, we could alloc memory for every read
       and return a copy of the data. */
#ifdef UA_MULTITHREADING
	pthread_rwlock_rdlock(&writeLock);
#endif
	value->value.type = &UA_TYPES[UA_TYPES_BOOLEAN];
	value->value.arrayLength = -1;
	value->value.data = &ledStatus;
	value->value.arrayDimensionsSize = -1;
	value->value.arrayDimensions = NULL;
	value->hasValue = UA_TRUE;
	if(sourceTimeStamp) {
		value->sourceTimestamp = UA_DateTime_now();
		value->hasSourceTimestamp = UA_TRUE;
	}
	return UA_STATUSCODE_GOOD;
}

static void releaseLedStatus(void *handle, UA_DataValue *value) {
    if(!value->hasValue)
        return;
	/* If we allocated memory for a specific read, free the content of the
       variantdata. */
	value->value.arrayLength = -1;
	value->value.data = NULL;
#ifdef UA_MULTITHREADING
	pthread_rwlock_unlock(&writeLock);
#endif
}

static UA_StatusCode writeLedStatus(void *handle, const UA_Variant *data, const UA_NumericRange *range) {
    if(range)
        return UA_STATUSCODE_BADINDEXRANGEINVALID;
    
#ifdef UA_MULTITHREADING
	pthread_rwlock_wrlock(&writeLock);
#endif
	if(data->data)
		ledStatus = *(UA_Boolean*)data->data;

	if(triggerFile)
		fseek(triggerFile, 0, SEEK_SET);

	if(ledFile){
		if(ledStatus == 1){
			fprintf(ledFile, "%s", "1");
		} else {
			fprintf(ledFile, "%s", "0");
		}
		fflush(ledFile);
	}
#ifdef UA_MULTITHREADING
	pthread_rwlock_unlock(&writeLock);
#endif
	return UA_STATUSCODE_GOOD;
}

static void stopHandler(int sign) {
    UA_LOG_INFO(logger, UA_LOGCATEGORY_SERVER, "Received Ctrl-C\n");
	running = 0;
}

static UA_ByteString loadCertificate(void) {
	UA_ByteString certificate = UA_STRING_NULL;
	FILE *fp = NULL;
	//FIXME: a potiential bug of locating the certificate, we need to get the path from the server's config
	fp=fopen("server_cert.der", "rb");

	if(!fp) {
        errno = 0; // we read errno also from the tcp layer...
        return certificate;
    }

    fseek(fp, 0, SEEK_END);
    certificate.length = ftell(fp);
    certificate.data = malloc(certificate.length*sizeof(UA_Byte));
	if(!certificate.data)
		return certificate;

    fseek(fp, 0, SEEK_SET);
    if(fread(certificate.data, sizeof(UA_Byte), certificate.length, fp) < (size_t)certificate.length)
        UA_ByteString_deleteMembers(&certificate); // error reading the cert
    fclose(fp);

    return certificate;
}

int main(int argc, char** argv) {
	signal(SIGINT, stopHandler); /* catches ctrl-c */
#ifdef UA_MULTITHREADING
	pthread_rwlock_init(&writeLock, 0);
#endif

	UA_Server *server = UA_Server_new(UA_ServerConfig_standard);
	logger = Logger_Stdout_new();
	UA_Server_setLogger(server, logger);
    UA_ByteString certificate = loadCertificate();
    UA_Server_setServerCertificate(server, certificate);
    UA_ByteString_deleteMembers(&certificate);
	UA_Server_addNetworkLayer(server, ServerNetworkLayerTCP_new(UA_ConnectionConfig_standard, 16664));

	// add node with the datetime data source
	UA_DataSource dateDataSource = (UA_DataSource)
        {.handle = NULL,
		.read = readTimeData,
		.release = releaseTimeData,
		.write = NULL};
	const UA_QualifiedName dateName = UA_QUALIFIEDNAME(1, "current time");
	UA_Server_addDataSourceVariableNode(server, dateDataSource, dateName, UA_NODEID_NULL,
                                        UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                                        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES));

	//cpu temperature monitoring for linux machines
	if((temperatureFile = fopen("/sys/class/thermal/thermal_zone0/temp", "r"))){
		// add node with the data source
		UA_DataSource temperatureDataSource = (UA_DataSource)
    	    {.handle = NULL,
			.read = readTemperature,
			.release = releaseTemperature,
			.write = NULL};
		const UA_QualifiedName tempName = UA_QUALIFIEDNAME(1, "cpu temperature");
		UA_Server_addDataSourceVariableNode(server, temperatureDataSource, tempName, UA_NODEID_NULL,
                                            UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                                            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES));
	}

	//LED control for rpi
	if(  access("/sys/class/leds/led0/trigger", F_OK ) != -1
	  || access("/sys/class/leds/led0/brightness", F_OK ) != -1){
        if (	(triggerFile = fopen("/sys/class/leds/led0/trigger", "w"))
            && 	(ledFile = fopen("/sys/class/leds/led0/brightness", "w"))) {
            //setting led mode to manual
            fprintf(triggerFile, "%s", "none");
            fflush(triggerFile);

            //turning off led initially
            fprintf(ledFile, "%s", "1");
            fflush(ledFile);

            // add node with the LED status data source
            UA_DataSource ledStatusDataSource = (UA_DataSource)
                {.handle = NULL,
                .read = readLedStatus,
                .release = releaseLedStatus,
                .write = writeLedStatus};
            const UA_QualifiedName statusName = UA_QUALIFIEDNAME(0, "status LED");
            UA_Server_addDataSourceVariableNode(server, ledStatusDataSource, statusName, UA_NODEID_NULL,
                                                UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                                                UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES));
        }else{
            UA_LOG_WARNING(logger, UA_LOGCATEGORY_USERLAND, "[Raspberry Pi] LED file exist, but I have no access (try to run server with sudo)");
        }
    }

	// add a static variable node to the adresspace
    UA_Variant *myIntegerVariant = UA_Variant_new();
    UA_Int32 myInteger = 42;
    UA_Variant_setScalarCopy(myIntegerVariant, &myInteger, &UA_TYPES[UA_TYPES_INT32]);
    const UA_QualifiedName myIntegerName = UA_QUALIFIEDNAME(1, "the answer");
    const UA_NodeId myIntegerNodeId = UA_NODEID_STRING(1, "the.answer");
    UA_NodeId parentNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId parentReferenceNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
    UA_Server_addVariableNode(server, myIntegerVariant, myIntegerName,
                              myIntegerNodeId, parentNodeId, parentReferenceNodeId);

   /**************/
   /* Demo Nodes */
   /**************/

#define DEMOID 50000
   UA_Server_addObjectNode(server,UA_QUALIFIEDNAME(1, "Demo"), UA_NODEID_NUMERIC(1, DEMOID), UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER), UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES), UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE));

#define SCALARID 50001
   UA_Server_addObjectNode(server,UA_QUALIFIEDNAME(1, "Scalar"), UA_NODEID_NUMERIC(1, SCALARID), UA_NODEID_NUMERIC(1, DEMOID), UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES), UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE));

#define ARRAYID 50002
   UA_Server_addObjectNode(server,UA_QUALIFIEDNAME(1, "Array"), UA_NODEID_NUMERIC(1, ARRAYID), UA_NODEID_NUMERIC(1, DEMOID), UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES), UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE));

   UA_UInt32 id = 51000; //running id in namespace 0
   for(UA_UInt32 type = 0; UA_IS_BUILTIN(type); type++) {
       if(type == UA_TYPES_VARIANT || type == UA_TYPES_DIAGNOSTICINFO)
           continue;
       //add a scalar node for every built-in type
        void *value = UA_new(&UA_TYPES[type]);
        UA_Variant *variant = UA_Variant_new();
        UA_Variant_setScalar(variant, value, &UA_TYPES[type]);
        char name[15];
        sprintf(name, "%02d", type);
        UA_QualifiedName myIntegerName = UA_QUALIFIEDNAME(1, name);
        UA_Server_addVariableNode(server, variant, myIntegerName, UA_NODEID_NUMERIC(1, ++id),
                                  UA_NODEID_NUMERIC(1, SCALARID), UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES));

        //add an array node for every built-in type
        UA_Variant *arrayvar = UA_Variant_new();
        UA_Variant_setArray(arrayvar, UA_Array_new(&UA_TYPES[type], 10), 10, &UA_TYPES[type]);
        UA_Server_addVariableNode(server, arrayvar, myIntegerName, UA_NODEID_NUMERIC(1, ++id),
                                  UA_NODEID_NUMERIC(1, ARRAYID), UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES));
   }

	//start server
	UA_StatusCode retval = UA_Server_run(server, 1, &running); //blocks until running=false

	//ctrl-c received -> clean up
	UA_Server_delete(server);

	if(temperatureFile)
		fclose(temperatureFile);

	if(triggerFile){
		fseek(triggerFile, 0, SEEK_SET);
		//setting led mode to default
		fprintf(triggerFile, "%s", "mmc0");
		fclose(triggerFile);
	}

	if(ledFile){
		fclose(ledFile);
	}

#ifdef UA_MULTITHREADING
	pthread_rwlock_destroy(&writeLock);
#endif

	return retval;
}
