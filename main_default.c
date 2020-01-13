
#include <LPC17xx.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include "context.h"

// global variables
uint8_t highestPriority = 0;
uint32_t msTicks = 0;
uint8_t bitVector = 0;

// struct to contain task control block (TCB) data
typedef struct TCB{
	uint8_t taskID; //task ID
	uint32_t taskStackPointer; //pointer to head of stack
	uint32_t taskBaseAddr; //task base address
	uint8_t state; //current task state
	uint8_t priority; //current task priority
	struct TCB *next; // pointer to next task in queue
} TCB_t;

TCB_t *taskRunning;	// global current running task
TCB_t tcbList[6]; 	// initialize array of 6 task control blocks

// queue struct for scheduler
typedef struct queue{
	TCB_t *head;
	uint8_t size;
} queue_t;

queue_t priorityQueue[6];	// initialize array of 6 queues for scheduler

// struct to contain semaphore data
typedef struct {
	int32_t s;
	queue_t waitList;
} semaphore_t;

semaphore_t semaphore;

// struct to contain mutex data
typedef struct {
	int32_t s;
	queue_t waitList;
	uint8_t owner;
	uint8_t prevPriority;
}  mutex_t;

mutex_t mutex;

//pointer to a function that takes a void * parameter and returns void
typedef void (*rtosTaskFunc_t)(void *args);

// initiates context swtich every ms
void SysTick_Handler(void) {
	msTicks++;
	SCB->ICSR |= (1 << 28);
}
void initPriorityQueue() {
	uint8_t i = 0;
	while(i<=5){
		priorityQueue[i].head = NULL;
		priorityQueue[i].size = 0;
		i = i + 1;
	}
}

// adds node to end of a queue
// if queue is not empty, iterate to the end and then set taskToAdd as the next node
// if queue it empty, set head = taskToAdd
void enqueue(queue_t *list, TCB_t *taskToAdd) {
	if (list->size > 0)	{ //check if not empty
		TCB_t *temp = list->head;
		while (temp->next != NULL) temp = temp->next;//iterate to end of queue
		temp->next = taskToAdd;	//set taskToAdd as the next node in the queue
	}
	else list->head = taskToAdd;	// if queue is empty, set taskToAdd as the head node
	taskToAdd->next = NULL; //set the pointer to the next task to null
	list->size = list->size + 1;	// increment size of list
	bitVector |= (1 << (taskToAdd->priority));  // update bit vector
}

// removes node (TCB node) from front of queue
// remove head from queue, setting the next node in the queue to be the head and returning the old head
TCB_t *dequeue(queue_t *list) {
	TCB_t *temp = list->head;
	list->head = list->head->next;
	if (list->size == 1) bitVector &= ~(1 << temp->priority);
	list->size = list->size - 1;
	return temp;
}

// remove specific node from queue
TCB_t *removeTCB(TCB_t *taskToDelete) {
	queue_t *list = &(priorityQueue[taskToDelete->priority]); // find the queue corresponding to the priority of taskToDelete

	// if taskToDelete is the head of the queue, use dequeue function
	if (list->head->taskID == taskToDelete->taskID) return dequeue(list);

	// if taskToDelete is not the head of the queue, iterate through the queue to find the node and remove it
	else{
		TCB_t *temp = list->head;
		while (temp->next != NULL) { //iterate through list to find TCB
			if (temp->next->taskID == taskToDelete->taskID) { // if TCB is found
				TCB_t *deletedNode = temp->next;
				temp->next = temp->next->next;	// remove node
				list->size = list->size - 1;	// decrement size
				return deletedNode;	// return removed node
			}
			temp = temp->next; // move to next node in list
		}
		// if taskToDelete not in queue, return NULL
		return NULL;
	}
}

// return index of queue with highest priority that is not empty
uint8_t highestPriorityQueue() {
	uint8_t leadingZeroCount = 0;
	leadingZeroCount = __CLZ(bitVector);
	uint8_t highestP = 31 - leadingZeroCount;
	return highestP;
}

//HELPER FUNCTIONS
//delay to slow down print statements for readability
void delay(){
	for(int delay = 0; delay < 15000000; delay++);
}

//pulse LEDs
void set_leds(uint32_t num){
	//set LEDs
	LPC_GPIO2->FIOSET |= (num & 1) << 6;
	LPC_GPIO2->FIOSET |= (num & 10) << 4;
	LPC_GPIO2->FIOSET |= (num & 100) << 2;
	LPC_GPIO2->FIOSET |= (num & 1000);
	LPC_GPIO2->FIOSET |= (num & 10000) >> 2;
	delay();
	//clear LEDs
	LPC_GPIO2->FIOCLR |= (num & 1) << 6;
	LPC_GPIO2->FIOCLR |= (num & 10) << 4;
	LPC_GPIO2->FIOCLR |= (num & 100) << 2;
	LPC_GPIO2->FIOCLR |= (num & 1000);
	LPC_GPIO2->FIOCLR |= (num & 10000) >> 2;
}

//MUTEX
//initialize mutex (count and wait list)
void initMutex(mutex_t *mutex){
	mutex->s = 1; //initialize mutex count to 1, showing the mutex is initially available
	mutex->waitList.head = NULL;
	mutex->waitList.size = 0;
}

//acquire mutex
void lock(mutex_t *mutex){
	__disable_irq();
	if (mutex->s){ // if mutex is not locked
		mutex->s = mutex->s - 1;
		mutex->owner = taskRunning->taskID;
		printf("Mutex Locked by Task %d\n", mutex->owner);
		mutex->prevPriority = tcbList[mutex->owner].priority; //Saving the priority of the owner in case of priority inheritance
	}
	else {
		if (taskRunning->priority > tcbList[mutex->owner].priority){ //Used for priority inheritance
			printf("Priority Inheritance: Priority of owner, Task %d, raised to %d from %d\n",mutex->owner, taskRunning->priority, tcbList[mutex->owner].priority);
			enqueue(&(priorityQueue[taskRunning->priority]), removeTCB(&(tcbList[mutex->owner])));
			tcbList[mutex->owner].priority = taskRunning->priority;
		}
		//If the task is not the owner, enqueue it into the waitlist to be run and set its state to 1
		if (taskRunning->taskID != mutex->owner){
			printf("Task %d Blocked, Waiting for Task %d to Release Mutex\n", taskRunning->taskID, mutex->owner);
			enqueue(&(mutex->waitList), taskRunning);
			taskRunning->state = 1;
		}
	}
	__enable_irq();
}

//release mutex
void release(mutex_t *mutex){
	__disable_irq();
	//Owner test on release
	if (mutex->owner == taskRunning->taskID){
		printf("Current owner of Mutex is %d\n", mutex->owner);
		//Restore old priority of the task if priority inheritance had occurred
		if (taskRunning->priority > mutex->prevPriority) taskRunning->priority = mutex->prevPriority;

		mutex->s = mutex->s + 1;

		if (mutex->waitList.size){
			//Dequeue the next task that needs to be run from the waitlist
			TCB_t *nextTask = dequeue(&mutex->waitList);
			//Mutex assigned to the next task, so count is decrement so lock identifies the mutex as assigned
			mutex->s = mutex->s - 1;
			//Enqueue the task that needs to be run into the running queue
			enqueue(&(priorityQueue[nextTask->priority]), nextTask);
			//Make the owner of the mutex now the task that is about to run
			mutex->owner = nextTask->taskID;
			//save priority of the next task to be run in case of priority inheritance
			mutex->prevPriority = nextTask->priority;
			//Set state to ready to be run
			nextTask->state = 2;
			// raise priority if necessary for priority inheritance
			printf("Task %d Enqueued and Mutex Assigned to Task %d -> ", nextTask->taskID, mutex->owner);

			if (nextTask->priority < highestPriority) nextTask->priority = highestPriority;

		}
		printf("Released by Task %d\n", taskRunning->taskID);
	}
	__enable_irq();
}

//SEMAPHORE
// initialize semaphore (count and wait list)
void initSemaphore(semaphore_t *sem, int32_t count){
	sem->waitList.size = 0;
	sem->waitList.head = NULL;
	sem->s = count;
}

// wait (to acquire semaphore)
void wait(semaphore_t *sem){
	__disable_irq();
	//If the stopping condition for semaphores is hit, enqueue the semaphore that needs to run into the waitlist and set its state to 1
	//This is where blocking semaphores occurs
	if (sem->s < 1) {
		enqueue(&sem->waitList, taskRunning);
		taskRunning->state = 1;
		printf("Wait called by Task %d\n", taskRunning->taskID);
	}
	sem->s = sem->s - 1;
	__enable_irq();
}

// singal (to release semaphore)
void signal(semaphore_t *sem){
	__disable_irq();
	//if count is less than 1 and signal is called, we are dequeuing the next task to be run from the waitlist and enqueuing it into the running queue
	//We also set the state of the task to ready since it has been added to the running queue
	if (sem->s < 1 && sem->waitList.size) {
		TCB_t *nextRunningTask = dequeue(&(sem->waitList));
		printf("Signal called by Task %d\n", nextRunningTask->taskID);
		enqueue(&(priorityQueue[nextRunningTask->priority]), nextRunningTask);
		nextRunningTask->state = 2;
	}
	sem->s = sem->s + 1;
	__enable_irq();
}

//CONTEXT SWITCHES
//context switch
void conSwitch(uint8_t newTask) {
	uint8_t oldTask = taskRunning->taskID;
	tcbList[oldTask].taskStackPointer = storeContext(); // put registers for running task into stack
	tcbList[oldTask].state = (tcbList[oldTask].state == 3) ? 2: 3;
	restoreContext(tcbList[newTask].taskStackPointer); // push new task stack contents to registers
}

// initialize + handle context switches
void PendSV_Handler(){
	uint8_t nextQueue = highestPriorityQueue(); //The next task inside the queue that has the highest priority needs to run

	// perform context switch if next queue is greater than or equal to priority of running task
	if (taskRunning->priority <= nextQueue) {
		TCB_t *tasknextRunningTask = dequeue(&priorityQueue[nextQueue]); // dequeue next ready task
		uint8_t temp1 = taskRunning->taskID;
    // enqueue running task unless waiting for semaphore to be available
		if (taskRunning->state != 1) enqueue(&priorityQueue[taskRunning->priority], taskRunning);
		// switch context between next task and running task
		conSwitch(tasknextRunningTask->taskID);
		taskRunning = tasknextRunningTask;
		SCB->ICSR &= ~(1 << 28); //Clears pendSV_Handler set bit
	}
}

//INITIALIZATION
// initialize
void init(void){
	uint32_t *vectorTable = 0;
	uint32_t mainStackStartAddress = vectorTable[0];

//Initializes each TCB with the base address for its stack
	uint8_t bit = 0;
	while(bit<=5){
		tcbList[bit].taskBaseAddr = mainStackStartAddress - 2048 - 1024 * (5-bit);
		tcbList[bit].taskID = bit;
		tcbList[bit].state = 0;
		tcbList[bit].taskStackPointer = tcbList[bit].taskBaseAddr;
		bit = bit + 1;
	}

	// move the main stack contents to the process stack of the new main
	uint32_t mainStackPointer = __get_MSP(); // copy main stack contents to process stack

	uint8_t count = 0;
	while(count<6){
		*(uint32_t *)tcbList[0].taskBaseAddr -= *(uint32_t *)count;
		*(uint32_t *)mainStackStartAddress -= *(uint32_t *)count;
		tcbList[0].taskBaseAddr = mainStackStartAddress;
		count = count + 1;
	}
	//memcpy(tcbList[0].taskBaseAddr-1024, mainStackStartAddress, 1024);
	__set_MSP(mainStackStartAddress);
	tcbList[0].priority = 0;
	tcbList[0].state = 3;
	//Locating and initializing the stack pointer for the task to the correct address using the address of the main stack pointer and base
	tcbList[0].taskStackPointer = tcbList[0].taskBaseAddr + mainStackPointer - mainStackStartAddress;

	//Init the running task to the base tcb block and the next to NULL until it is enqueued later
	taskRunning = &tcbList[0];
	taskRunning->next = NULL;

	//Switching from using the MSP to the PSP by changing SPSEL using control register
	__set_CONTROL(__get_CONTROL() | 2);
	//PSP address is currently invalid, so we change it to the top of the stack selected for the main task
	__set_PSP(tcbList[0].taskStackPointer);
}

uint8_t createTask(rtosTaskFunc_t functionPointer, void *args, uint8_t priority) {
	uint8_t i = 1;

	// find next empty stack
	while(tcbList[i].taskStackPointer != tcbList[i].taskBaseAddr) {
		i = i + 1;
		if (i > 5) return 0;
	}

	tcbList[i].state = 2; // set it to ready to run
	tcbList[i].priority = priority; // set task's priority

	//Update highest priority
	//highestPriority = (highestPriority < priority) ? priority:highestPriority
	if(priority>highestPriority) highestPriority = priority;

	for(uint8_t j = 0; j<16;j++){
		tcbList[i].taskStackPointer -= 4;
		if(j==0) *((uint32_t *)tcbList[i].taskStackPointer) = (uint32_t)0x01000000;
		else if(j==1) *((uint32_t *)tcbList[i].taskStackPointer) = (uint32_t)functionPointer;
		else if(j>1 && j<=6) *((uint32_t *)tcbList[i].taskStackPointer) = (uint32_t)0;
		else if (j==7) *((uint32_t *)tcbList[i].taskStackPointer) = *(uint32_t *)args;
		else *((uint32_t *)tcbList[i].taskStackPointer) = (uint32_t)0;
	}

	TCB_t * enqueuedTask = &tcbList[i]; // create a new node for the task
	enqueue(&priorityQueue[tcbList[i].priority], enqueuedTask);

	return 1;
}

//TASKS
//task 4: output 4 in + to LEDs
void task4(void *s){
	while(1) {
		lock(&mutex);
		__disable_irq();
		delay();
		printf("...TASK 4 STARTED...\n");
		__enable_irq();
		set_leds(4);
		release(&mutex);
		__disable_irq();
		printf("...TASK 4 DONE...\n");
		delay();
		__enable_irq();
	}
}

//TASK 1: output 1 in binary to LEDs
// task 1 mutex
void task1Mtx(void *s){
	while(1) {
		lock(&mutex);
		__disable_irq();
		delay();
		printf("...TASK 1 STARTED...\n");
		__enable_irq();
		set_leds(1);
		release(&mutex);
		__disable_irq();
		printf("...TASK 1 DONE...\n");
		delay();
		__enable_irq();
	}
}

//task 1 sem
void task1Sem(void *s){
	while(1) {
		wait(&semaphore);
		__disable_irq();
		printf("...TASK 1 STARTED...\n");
		__enable_irq();
		set_leds(1);
		signal(&semaphore);
		__disable_irq();
		printf("...TASK 1 DONE...\n");
		delay();
		__enable_irq();
	}
}

//TASK 2: output 2 in binary to LEDs
// task 2 mutex
void task2Mtx(void *s){
	while(1) {
		lock(&mutex);
		__disable_irq();
		delay();
		printf("...TASK 2 STARTED...\n");
		__enable_irq();
		set_leds(2);
		release(&mutex);
		__disable_irq();
		printf("...TASK 2 DONE...\n");
		delay();
		__enable_irq();
	}
}

// task 2 semaphore
void task2Sem(void *s){
	while(1) {
		wait(&semaphore);
		__disable_irq();
		printf("...TASK 2 STARTED...\n");
		__enable_irq();
		set_leds(2);
		signal(&semaphore);
		__disable_irq();
		printf("...TASK 2 DONE...\n");
		delay();
		__enable_irq();
	}
}

//TASK 3: output 3 in binary to LEDs
// task 3 semaphore
void task3Sem(void *s){
	while(1) {
		wait(&semaphore);
		__disable_irq();
		delay();
		printf("...TASK 3 STARTED...\n");
		__enable_irq();
		set_leds(2);
		signal(&semaphore);
		__disable_irq();
		printf("...TASK 3 DONE...\n");
		delay();
		__enable_irq();
	}
}

//task 3 mutex
uint8_t task3Count = 0;
void task3Mtx(void* s){	// blink LED 4
	while(1) {
		lock(&mutex);
		task3Count++;
		__disable_irq();
		printf("...TASK 3 STARTED...\n");
		__enable_irq();
		set_leds(3);
		release(&mutex);
		__disable_irq();
		printf("...TASK 3 DONE...\n");
		delay();
		__enable_irq();
		//When the count is 2, we create task 4 which is at the highest possible priority; this raises the current owners priority until the task
		//is finished, and when it is finished task 4 starts and never ends since it is the highest priority and never yields
		if (task3Count == 1) {
			__disable_irq();
			printf("TASK 4 CREATED\n");
			__enable_irq();
			rtosTaskFunc_t p4 = task4;
			createTask(p4, s, 2);
		}
	}
}

//TEST SUITE
void blockingSemaphoreTest(void){
	initSemaphore(&semaphore, 2);
	char *c = "give us 100";
	rtosTaskFunc_t p1 = task1Sem;
	rtosTaskFunc_t p2 = task2Sem;
	rtosTaskFunc_t p3 = task3Sem;
	createTask(p1, c, 1);
	createTask(p2, c, 1);
	createTask(p3, c, 1);
}

void notBlockingSemaphoreTest(void){
	initSemaphore(&semaphore, 8);
	char *c = "give us 100";
	rtosTaskFunc_t p1 = task1Sem;
	rtosTaskFunc_t p2 = task2Sem;
	rtosTaskFunc_t p3 = task3Sem;
	createTask(p1, c, 1);
	createTask(p2, c, 1);
	createTask(p3, c, 1);
}

void contextSwtich_OwnerTestOnRelease_Test(void){
	initMutex(&mutex);
	char *c = "give us 100";
	rtosTaskFunc_t p1 = task1Mtx;
	rtosTaskFunc_t p2 = task2Mtx;
	createTask(p1, c, 1);
	createTask(p2, c, 1);
}


int main(void) {
	printf("\033\143"); //used to clear putty terminal

	// system initializations
	SysTick_Config(SystemCoreClock/1000);
	init();
	initPriorityQueue();
	initMutex(&mutex);
	initSemaphore(&semaphore, 2);

	//initialize and clear LEDs
	LPC_GPIO1->FIODIR |= ((uint32_t)11<<28);
	LPC_GPIO1->FIOCLR |= ((uint32_t)11<<28);
	LPC_GPIO2->FIODIR |= 0x0000007C;
	LPC_GPIO2->FIOCLR |= 0x0000007C;

	char *c = "give us 100";

	//TEST SUITE
	//blockingSemaphoreTest();
	//notBlockingSemaphoreTest();
	//contextSwtich_OwnerTestOnRelease_Test();
	

	// ALL MUTEXT TESTs (FPP, Priority Inheritance, Context Switch, Owner Test on Release)
	rtosTaskFunc_t p1 = task1Mtx;
	rtosTaskFunc_t p2 = task2Mtx;
	rtosTaskFunc_t p3 = task3Mtx;
	createTask(p1, c, 0);
	createTask(p2, c, 1);
	createTask(p3, c, 1);





	while(1);
}
