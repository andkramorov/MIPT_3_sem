#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_ALLOWED 1
#define TESTING 0
#define SHM_KEY 3145
#define SEM_KEY 271828
#define BUFFER_SIZE 4096
#define WRITE_SEM 0
#define READ_SEM 1
#define EXCL_SEM 2
#define USERCOUNT_SEM 3

#ifndef SEMUN_H
#define SEMUN_H
union semun { /* Used in calls to semctl() */
 int val;
 struct semid_ds * buf;
 unsigned short * array;
#if defined __linux__
 struct seminfo * __buf;
#endif
#endif // SEMUN_H
};
// returns semaphore set id
int get_sem_id(int key, int sem_num, int* initial_values);
int sem_reserve(int semid, int num);
int sem_timeout_reserve(int semid, int num);
int sem_release(int semid, int num);
int sem_op(int semid, int num, int value);
int sem_delete(int semid, int sem_num);
int consistency_check(int semid);
int main() {
    int server_sem_id, client_sem_id ,server_shm_id, client_shm_id, count, v;
    void * server_address, * client_address;
    int server_sem_initial_val[2] = {1, 0};
    union semun arg;
    //getting server_sem_id
    server_sem_id = get_sem_id(SEM_KEY, 2 ,server_sem_initial_val);
    if(server_sem_id == -1) {
        perror(" Can't get server sem id");
        exit(EXIT_FAILURE);
    }
    // attaching server shared memory
    server_shm_id = shmget(SHM_KEY, 2*sizeof(int), IPC_CREAT | S_IWUSR | S_IRUSR);
    if(server_shm_id == -1) {
        perror(" Error getting shmid ");
        sem_delete(server_sem_id, 2);
        exit(EXIT_FAILURE);
    }
    server_address = shmat(server_shm_id, 0, 0);
    v = consistency_check(server_sem_id); // checking consistency of semaphore set
    if(v == -1) {
        fprintf(stderr, " Consistency check failed, aborting");
        shmdt(server_address);
        semctl(server_sem_id, 0,  IPC_RMID, 0);
        exit(EXIT_FAILURE);
    }
    if(v == 1) {
        fprintf(stderr, " Consistency check failed, semaphore set consistent state reestablished, continuing ");
    }// if v == 0, semaphore set is in consistent state, continuing
    for(;;) {
        // acquiring client semaphore set
        client_sem_id = semget(IPC_PRIVATE, 2, IPC_CREAT | O_EXCL | S_IWUSR | S_IRUSR);
        if(client_sem_id == -1) {
            perror(" Can't get client sem id");
            shmdt(server_address);
            sem_delete(server_sem_id, 2);
            exit(EXIT_FAILURE);
        }
        arg.val = 0;
        semctl(client_sem_id, READ_SEM, SETVAL, arg);
        arg.val = 1;
        semctl(client_sem_id, WRITE_SEM, SETVAL, arg);
        //attaching  client shared memory
        client_shm_id = shmget(IPC_PRIVATE, BUFFER_SIZE + sizeof(int), IPC_CREAT | O_EXCL | S_IWUSR | S_IRUSR);
        if(client_shm_id == -1) {
            perror(" Can't create shared memory ");
            shmdt(server_address);
            semctl(client_sem_id, 0 , IPC_RMID, 0);
            sem_delete(server_sem_id, 2);
            exit(EXIT_FAILURE);
        }
        client_address = shmat(client_shm_id, 0, 0);
        // writing to the server shared memory
        sem_reserve(server_sem_id, WRITE_SEM);
        sem_reserve(server_sem_id, EXCL_SEM); // so no other process can enter semaphore; 
        //if process crashes before releasing WRITE_SEM, all changes to semaphore set will be reverted
        *(int *) server_address = client_shm_id;
        *(((int *) server_address) + 1) = client_sem_id;
        arg.val = 0;
        semctl(server_sem_id, WRITE_SEM, SETVAL, arg);
        arg.val = 1;
        semctl(server_sem_id, READ_SEM, SETVAL, arg);
        semctl(server_sem_id, EXCL_SEM, SETVAL, arg); // so SEM_UNDO flag on sem_reserve(server_sem_id,READ_SEM) will not undo the changes if process crashes
        // this is done to ensure that semaphore set stays in consistent state: no doing so may result in 1 1 state of WRITE_SEM and READ_SEM
        //(if we release WRITE_SEM, anoother process starts, and then our program crashes, kernel won't be able to undo all the changes
        // wich will result in 1 READ_SEM, 0 WRITE_SEM and another process in the semaphore set, wich will eventually result in 1 1 state)
        // reading from  client shared memory
        sem_reserve(client_sem_id, READ_SEM); // waiting for a writing process to appear, no timeout required
        count = * (int *) client_address;
        if(count == 0) {
            shmdt(client_address);
            semctl(client_sem_id, 0, IPC_RMID, 0);
            break;
        } // there is nothing to read
        write(STDOUT_FILENO, (((char *) client_address)+ sizeof(int)), count);// else, perform a single write operation after which we enter reading loop
        sem_release(client_sem_id, WRITE_SEM);
        for(;;) {
            if(sem_timeout_reserve(client_sem_id, READ_SEM) == -1) { // writing process is either terminated or frozen
                perror(" Timeout exception , begining writing to the server shared memory ");
                count = -1; // error during reading from client shared memory
                break;   
            }
            count = * (int *) client_address;
            if(count == 0)
                break; // all data has been successfully read
            write(STDOUT_FILENO, (((char *) client_address)+ sizeof(int)), count);
            sem_release(client_sem_id, WRITE_SEM);
        }
        shmdt(client_address);
        semctl(client_sem_id, 0, IPC_RMID, 0);
        if(count == 0)
            break;
    }
    sem_delete(server_sem_id, 2);
    shmdt(server_address);
}
#ifdef __linux__
int sem_timeout_reserve(int semid, int num) {
    struct sembuf buf;
    struct timespec time;
    time.tv_sec = 10;
    time.tv_nsec = 0;
    buf.sem_num = num;
    buf.sem_op = -1;
    buf.sem_flg = SEM_UNDO;
    while(semtimedop(semid, &buf, 1, &time) == -1) {
        if(errno == EAGAIN) {
            perror(" Timeout exception ");
            return -1;
        }
        if(errno != EINTR) {
            perror("Error during semop");
            return -1;
        }
    }
    return 0;
}
#else
int sem_timeout_reserve(int semid, int num) {
    return sem_reserve(semid, num);
}
#endif // __linux__
int sem_op(int semid, int num, int value) {
    struct sembuf buf;
    buf.sem_num = num;
    buf.sem_op = value;
    buf.sem_flg = SEM_UNDO;
    while(semop(semid, &buf, 1) == -1) {
        if(errno != EINTR) {
            perror("Error during semop");
            return -1;
        }
    }
    return 0;
}
int sem_release(int semid, int num) {
    struct sembuf buf;
    buf.sem_num = num;
    buf.sem_op = 1;
    buf.sem_flg = SEM_UNDO;
    while(semop(semid, &buf, 1) == -1) {
        if(errno != EINTR) {
            perror("Error during semop");
            return -1;
        }
    }
    return 0;
}
int sem_reserve(int semid, int num) {
    struct sembuf buf;
    buf.sem_num = num;
    buf.sem_op = -1;
    buf.sem_flg = SEM_UNDO;
    while(semop(semid, &buf, 1) == -1) {
        if(errno != EINTR) {
            perror("Error during semop");
            return -1;
        }
    }
    return 0;
}
int consistency_check(int semid) {
    int sem_num, i, zero, first, second, third, error;
    union  semun arg;
    struct  semid_ds sem;
    arg.buf = &sem;
    error = 0;
    arg.val = 0;
    semctl(semid, 0 , IPC_STAT, arg);
    sem_num = sem.sem_nsems -2;
    if(sem_timeout_reserve(semid, EXCL_SEM) == -1) {
        perror(" Semaphore set is not in consistent state, correction is impossible");
        return -1;
    }
    for(i = 0; i < sem_num + 1; i++) {
        zero = semctl(semid, i, GETVAL, 0);
        if(zero > 1) {
            semctl(semid, 0, SETVAL, arg);
            error = 1;
        }
    }
    zero = semctl(semid, 0, GETVAL, 0);
    first = semctl(semid, 1, GETVAL, 0);
    second = semctl(semid, 2, GETVAL, 0);
    third = semctl(semid, 3, GETVAL, 0);
    arg.val = 1;
    if(zero == 0 && first == 0) {
        semctl(semid, 0, SETVAL, arg);
        error = 1;
    }
    else if (zero == 1 && first == 1) {
        arg.val = 0;
        semctl(semid, 1, SETVAL, arg);
        error = 1;
    }
    if(error == 1) {
        fprintf(stderr, " Error in semaphore set detected, printing semaphore values after correction");
        for(i =0; i < sem_num+ 2; i++) 
            fprintf(stderr, " semaphore %d value %d", i, semctl(semid, i, GETVAL, 0));
        sem_release(semid, EXCL_SEM);
        return 1;
    }
    sem_release(semid, EXCL_SEM);
    return 0;
}
int get_sem_id(int key, int sem_num, int * initial_values) {
    int semid, i;
    semid = semget(key, sem_num + 2, IPC_CREAT | IPC_EXCL | S_IWUSR | S_IRUSR);
    if (semid != -1) { /* Successfully created the semaphore */
        union semun arg;
        struct sembuf sop;
        for(i = 0; i < sem_num; i++) { // initializing semaphore values
            arg.val = initial_values[i];
            semctl(semid, i, SETVAL, arg);
        }
        arg.val = 1;
        semctl(semid, sem_num+1, SETVAL, arg);// initializing semaphore , that counts number of attached processes
        arg.val = 0;
        semctl(semid, sem_num, SETVAL, arg); // initializing exclusive semaphore
        sop.sem_num = sem_num;
        sop.sem_op = 1;
        sop.sem_flg = 0;
        semop(semid, &sop, 1); // set sem_otime
        return semid;
    }/* We didn't create the semaphore set */
    else  if(errno != EEXIST) {
            perror(" Error in first semget ");
            return -1;
    }
    else {
        struct semid_ds sem;
        union semun arg;
        semid = semget(key, sem_num+2, S_IWUSR | S_IRUSR);
        if (semid == -1) {
            if(errno == ENOENT) // semaphore set was deleted, trying again
                return get_sem_id(key, sem_num, initial_values);
            perror(" Error retrieving existing semaphore set");
            return -1;
        }
        #if TIMEOUT_ALLOWED
        arg.buf = &sem;
        semctl(semid, 0 , IPC_STAT, arg);
        if(sem.sem_otime) { // semaphore set was initialized
            if(sem_op(semid, sem_num, -1) == -1){ //reserving semaphore set;  if it was unitialized, process will block until it will be
                // semaphore set was removed while we were performing sem_op(or before); restart the function to create a new set
                if(errno == EIDRM)
                    return get_sem_id(key, sem_num, initial_values);
                perror(" Can't reserve semaphore set"); // other semop error
                return -1;
            }
        }
        else if(sem_timeout_reserve(semid, sem_num) == -1) {
            if(errno == EAGAIN) {
                perror(" Initialisation failure, deleting semaphore set");
                semctl(semid, 0, IPC_RMID, 0);
                return get_sem_id(key, sem_num, initial_values);
            }
            if(errno == EIDRM)
                return get_sem_id(key, sem_num, initial_values);
            perror(" Can't reserve semaphore set"); // other semop error
            return -1;  
        }
        #else
        if(sem_op(semid, sem_num, -1) == -1){ //reserving semaphore set;  if it was unitialized, process will block until it will be
            // semaphore set was removed while we were performing sem_op(or before); restart the function to create a new set
            if(errno == EIDRM)
                return get_sem_id(key, sem_num, initial_values);
            perror(" Can't reserve semaphore set"); // other semop error
            return -1;
        }
        #endif
        sem_release(semid, sem_num+1);//attaching semaphore set
        sem_op(semid, sem_num, 1); // releasing exclusive semaphore
        return semid;
    }
    return -1;
}
int sem_delete(int semid, int sem_num) {
    sem_reserve(semid, sem_num+1); // detaching from semaphore set
    if(sem_timeout_reserve(semid, sem_num) == -1) {// reserving semaphore set, so no other semaphores can be attached
        perror(" Semaphore set is in inconsistent state, deleting semaphore set");
        if(semctl(semid, sem_num, IPC_RMID, 0) == -1) { // if semaphore set can't be deleted
            fprintf(stderr, " Can't delete sepamphore, id : %d", semid);
            perror(" ");
            return -1;
        }
        return 0;
    } 
    if(semctl(semid, sem_num+1,GETVAL,0) == 0) { // no other processes are attached to the semaphore set
        if(semctl(semid, sem_num, IPC_RMID, 0) == -1) { // if semaphore set can't be deleted
            fprintf(stderr, " Can't delete sepamphore, id : %d", semid);
            perror(" ");
            return -1;
        }
        return 0; // else : successfuly deleted he semaphore
    }
    else{ // it is not required to delete the semaphore
        sem_op(semid, sem_num, 1); //releasing semaphore set
        return 0;
    }
    return -1;
}
