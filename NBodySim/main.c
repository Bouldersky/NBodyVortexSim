//
//  main.c
//  NBodySim
//
//

#include "main.h"
#include "guiOutput.h"
#include "constants.h"
#include "TestCaseInitializers.h"
#include "fileIO.h"
#include "RNG.h"
#include "C-Thread-Pool/thpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <assert.h>
#include <string.h>
#include <limits.h>
#include <signal.h>

#undef DEBUG

unsigned int randomSeed;
double timestep;
int currentTimestep;
int numDriverVorts;
threadpool thpool;

#pragma mark - Index Calculators

/**
  This function return the index in the vortex radii array for the radius between a pair of vortices

  @param vortIndex1 the index of one of the vortices. Can be found by examining the vIndex value of the vortex in question
  @param vortIndex2 the index of the other vortex

  @return Index of the radius value
  */
long calculateVortexRadiiIndex(long vortIndex1, long vortIndex2) {
    // radii is basically a hash table, with this as the hash function.
    // I suspect that this is better than the standard dictionary libraries for this problem.
    // I'm able to guarentee that the structure is full, that there are no
    // collisions, and I'm able to iterate efficiently by incrementing an array pointer.
    if (vortIndex1 < vortIndex2) {
        return ((vortIndex2-1)*vortIndex2/2 + vortIndex1) * 3;
    } else {
        return ((vortIndex1-1)*vortIndex1/2 + vortIndex2) * 3;
    }
}

/**
  Calculates the index in the tracer radii array for the radius between a specific tracer and a specific vortex

  @param tracerIndex the index of the tracer. Can be found by exmaining the tIndex of the tracer in question
  @param vortIndex the index of the vortex. Can be found by examining the vIndex of the vortex in question
  @return The index in the tracer radii array of the radius
  */

long calculateTracerRadiiIndex(long tracerIndex, long vortIndex) {

    /*
       rows = tracers
       cols = vortices
       */

    // return (vortIndex * numTracers + tracerIndex)*3;
    return (tracerIndex * numDriverVorts + vortIndex) * 3;
}

#pragma mark - Math Functions

/**
  Recalculates all radii in a vortex radii array and a tracer radii array. Does so by calculating the pythagorean theorem for every radius. The radii arrays which are passed are modified.

  @param vortexRadii a pointer to the beginning of the array of doubles representing the radii of the vortices
  @param vortices a pointer to the beginning of the array of pointers to Vortex structs
  @param tracerRadii a pointer to the beginning of the array of doubles representing the radii between the tracers and vortices
  @param tracers a pointer to the array of Tracer structs
  @param numTracers the number of tracers which should be recalculated
  */
void updateRadii_pythagorean(double *vortexRadii, struct Vortex *vortices, double *tracerRadii, struct Tracer *tracers, int numTracers) {
    long index;

    for (int i = 0; i < numDriverVorts; ++i) { // if this doesn't segfault it'll be a goddamn miracle
        for (int j = 0; j < i; ++j) {
            index = calculateVortexRadiiIndex(vortices[i].vIndex, vortices[j].vIndex);

            vortexRadii[index+1] = vortices[i].position[0] - vortices[j].position[0];
            vortexRadii[index+2] = vortices[i].position[1] - vortices[j].position[1];
            vortexRadii[index] = sqrt(pow(vortexRadii[index+1], 2.) + pow(vortexRadii[index+2], 2.));
        }
    }
    index = 0;
    for (int tracerIndex = 0; tracerIndex < numTracers; tracerIndex++) {
        struct Tracer *tracer = &tracers[tracerIndex];
        for (int vortIndex = 0; vortIndex < numDriverVorts; vortIndex++) {
            struct Vortex *vort = &vortices[vortIndex];
            index = calculateTracerRadiiIndex(tracerIndex, vortIndex);

            tracerRadii[index+1] = vort->position[0] - tracer->position[0];
            tracerRadii[index+2] = vort->position[1] - tracer->position[1];
            tracerRadii[index] = sqrt(pow(tracerRadii[index+1], 2.) + pow(tracerRadii[index+2], 2.));
        }
    }
}

/**
  Calculate the strength of the interaction of one body on a second body as a function of the distance between the current body and the 2nd one.

  @param vortex2Intensity The intensity of the other vortex
  @param radius The radial distance to the other vortex
  @return the magnitude of the velocity vector resulting from the interaction of the other vortex on the current vortex
  */
double velocityFunc(double vortex2Intensity, double radius) {
    return vortex2Intensity/(2.*M_PI*radius);
    // return vortex2Intensity/radius;
}

#pragma mark - RK4 Functions

const char domains = 8; // 0 to disable wrapping of forces, 8 to enable it.
/**
  Calculate the x and y components of the velocity of a vortex based on the strengths of the other vortices, and the distance data in the intRads array
  @param xVel Pointer to a double which will be updated to reflect the new x-velocity of the vortex
  @param yVel Pointer to a double which will be updated to reflect the new y-velocity of the vortex
  @param vort Pointer to the vortex to compute velocities for
  @param vortices Pointer to an array of all of the vortices in the system
  @param rads The array containing all of the distance information between vortices as doubles
  @param numRads The length of the rads array
  */
void calculateVel_vortex(double *xVel, double *yVel, struct Vortex *vort, struct Vortex *vortices, double *rads, long numRads) {
    for (int j = 0; j < numDriverVorts; ++j) {
        if (vort->vIndex == j) {
            continue;
        }

        long radiiIndex = calculateVortexRadiiIndex(vort->vIndex, vortices[j].vIndex);

        for (int domain = 0; domain <= domains; domain++) {
            double rad;
            double xRad = (vort->vIndex < vortices[j].vIndex) ? rads[radiiIndex + 1] : -rads[radiiIndex + 1];
            double yRad = (vort->vIndex < vortices[j].vIndex) ? rads[radiiIndex + 2] : -rads[radiiIndex + 2];

            if (domain) {
                /* domain numbering scheme:
                   1 2 3
                   4 0 5
                   6 7 8
                   */

                switch (domain) {
                    case 1: {
                                xRad -= DOMAIN_SIZE_X;
                                yRad += DOMAIN_SIZE_Y;
                                break;
                            } case 2: {
                                yRad += DOMAIN_SIZE_Y;
                                break;
                            } case 3: {
                                xRad += DOMAIN_SIZE_X;
                                yRad += DOMAIN_SIZE_Y;
                                break;
                            } case 4: {
                                xRad -= DOMAIN_SIZE_X;
                                break;
                            } case 5: {
                                xRad += DOMAIN_SIZE_X;
                                break;
                            } case 6: {
                                xRad -= DOMAIN_SIZE_X;
                                yRad -= DOMAIN_SIZE_Y;
                                break;
                            } case 7: {
                                yRad -= DOMAIN_SIZE_Y;
                                break;
                            } case 8: {
                                xRad += DOMAIN_SIZE_X;
                                yRad -= DOMAIN_SIZE_Y;
                                break;
                            } default: {
                                break;
                            }
                }
                rad = sqrt(pow(xRad, 2) + pow(yRad, 2));
            } else {
                rad = rads[radiiIndex];
            }

            if (rad > DOMAIN_SIZE_X) {
                continue; // domain truncation
            }

            double vmag = velocityFunc(vortices[j].intensity, rad); // total velocity magnitude
            *xVel +=  (yRad/rad) * vmag;
            *yVel += (-xRad/rad) * vmag;
        }
    }
}

/**
  Calculate the x and y components of the velocity of a tracer based on the strengths of the vortices and their distances from the tracer
  @param xVel Pointer to a double which will be updated to reflect the new x-velocity of the vortex
  @param yVel Pointer to a double which will be updated to reflect the new y-velocity of the vortex
  @param tracerIndex index of the tracer to compute velocities for
  @param rads The array containing all of the distance information between tracers and vortices as doubles
  @param numRads The length of the rads array
  */
void calculateVel_tracer(double *xVel, double *yVel, long tracerIndex, double *rads, long numRads, struct Vortex *vortices) {
    for (int vortIndex = 0; vortIndex < numDriverVorts; vortIndex++) {
        long intRadIndex = calculateTracerRadiiIndex(tracerIndex, vortIndex);

        for (int domain = 0; domain <= domains; domain++) {
            double rad;
            double xRad = rads[intRadIndex + 1], yRad = rads[intRadIndex + 2];


            // if we're computing the effect of a vortex in the inner "driver" domain, then the radius data is already in memory. If
            // the other vortex is in one of the outter domains, then we need to compute the radius data by adding/subtracting the
            // domain size from its radius data, then compute the magnitude of the radius vector
            if (domain) {
                switch (domain) {
                    case 1: {
                        xRad -= DOMAIN_SIZE_X;
                        yRad += DOMAIN_SIZE_Y;
                        break;
                    } case 2: {
                        yRad += DOMAIN_SIZE_Y;
                        break;
                    } case 3: {
                        xRad += DOMAIN_SIZE_X;
                        yRad += DOMAIN_SIZE_Y;
                        break;
                    } case 4: {
                        xRad -= DOMAIN_SIZE_X;
                        break;
                    } case 5: {
                        xRad += DOMAIN_SIZE_X;
                        break;
                    } case 6: {
                        xRad -= DOMAIN_SIZE_X;
                        yRad -= DOMAIN_SIZE_Y;
                        break;
                    } case 7: {
                        yRad -= DOMAIN_SIZE_Y;
                        break;
                    } case 8: {
                        xRad += DOMAIN_SIZE_X;
                        yRad -= DOMAIN_SIZE_Y;
                        break;
                    } default: {
                        break;
                    }
                }
                rad = sqrt(pow(xRad, 2) + pow(yRad, 2));
            } else {
                rad = rads[intRadIndex];
            }

            // domain truncation
            if (rad > DOMAIN_SIZE_X || (TEST_CASE == 6 && rad < .1)) {
                continue;
            }

            double vmag = velocityFunc(vortices[vortIndex].intensity, rad);
            *xVel +=  (yRad/rad) * vmag;
            *yVel += (-xRad/rad) * vmag;
        }
    }
}

void stepForwardTracerRK4(void *arguments) {
    struct TracerArgs *args = arguments;
    int RKStep = args->RKStep;
    struct Tracer *tracers = args->tracers;
    int numTracers = args->numTracers;
    double *tracerRadii = args->tracerRadii;
    double *intermediateTracerRads = args->intermediateTracerRads;
    struct Vortex *vortices = args->vortices;

    for (int tracerIndex = 0; tracerIndex < numTracers; ++tracerIndex) {
        double k1_x = 0, k2_x = 0, k3_x = 0, k4_x = 0;
        double k1_y = 0, k2_y = 0, k3_y = 0, k4_y = 0;

        struct Tracer *tracer = &tracers[tracerIndex];
        double xVel = 0;
        double yVel = 0;

        long offsetTracerIndex = tracer->tIndex - tracers[0].tIndex;
        calculateVel_tracer(&xVel,
                &yVel,
                offsetTracerIndex,
                intermediateTracerRads,
                (numDriverVorts * numTracers),
                vortices);

        switch (RKStep) {
            case 1: {
                        k1_x += xVel;
                        k1_y += yVel;

                        xVel = xVel/2;
                        yVel = yVel/2;
                        break;
                    }
            case 2: {
                        k2_x += xVel;
                        k2_y += yVel;

                        xVel = xVel/2;
                        yVel = yVel/2;
                        break;
                    }
            case 3: {
                        k3_x += xVel;
                        k3_y += yVel;

                        break;
                    }
            case 4: {
                        k4_x += xVel;
                        k4_y += yVel;
                        break;
                    }
            default: {
                         break;
                     }
        }
#ifdef DEBUG

        switch (RKStep) {
            case 1: {
                        printf("step: %i | tracer: %i | k1_x: %1.15f\n", currentTimestep, tracer->tIndex, k1_x);
                        printf("step: %i | tracer: %i | k1_y: %1.15f\n", currentTimestep, tracer->tIndex, k1_y);
                        break;
                    }
            case 2: {
                        printf("step: %i | tracer: %i | k2_x: %1.15f\n", currentTimestep, tracer->tIndex, k2_x);
                        printf("step: %i | tracer: %i | k2_y: %1.15f\n", currentTimestep, tracer->tIndex, k2_y);
                        break;
                    }
            case 3: {
                        printf("step: %i | tracer: %i | k3_x: %1.15f\n", currentTimestep, tracer->tIndex, k3_x);
                        printf("step: %i | tracer: %i | k3_y: %1.15f\n", currentTimestep, tracer->tIndex, k3_y);
                        break;
                    }
            case 4: {
                        printf("step: %i | tracer: %i | k4_x: %1.15f\n", currentTimestep, tracer->tIndex, k4_x);
                        printf("step: %i | tracer: %i | k4_y: %1.15f\n", currentTimestep, tracer->tIndex, k4_y);
                        break;
                    }
            default: {
                         break;
                     }
        }
#endif

        for (int vortexIndex = 0; vortexIndex < numDriverVorts; ++vortexIndex) {
            long radIndex = calculateTracerRadiiIndex(offsetTracerIndex, vortexIndex);
            intermediateTracerRads[radIndex + 1] = tracerRadii[radIndex + 1] - xVel * timestep;
            intermediateTracerRads[radIndex + 2] = tracerRadii[radIndex + 2] - yVel * timestep;

            // pythagorean.
            intermediateTracerRads[radIndex] = sqrt(pow(intermediateTracerRads[radIndex+1], 2) + pow(intermediateTracerRads[radIndex+2], 2));
        }

        tracer->velocity[0] += (k1_x + k2_x * 2. + k3_x * 2. + k4_x)/6.;
        tracer->velocity[1] += (k1_y + k2_y * 2. + k3_y * 2. + k4_y)/6.;
    }

    free(arguments);
}

struct RKPositions *intPositionCache;
void stepForwardVortexRK4(void *arguments) {
    struct VortexArgs *args = arguments;
    struct Vortex *vortices = args->vortices;
    int originVortIndex = args->originVortIndex;
    double *intermediateRadii = args->intermediateRadii;
    double *workingRadii = args->workingRadii;
    long vortRadLen = args->vortRadLen;
    int RKStep = args->RKStep;
    double *intermediateTracerRads = args->intermediateTracerRads;
    int numTracers = args->numTracers;

    double k1_x = 0, k2_x = 0, k3_x = 0, k4_x = 0;
    double k1_y = 0, k2_y = 0, k3_y = 0, k4_y = 0;

    struct Vortex *vort = &vortices[originVortIndex];
    int vortIndex = vort->vIndex;

    double xVel = 0;
    double yVel = 0;

    calculateVel_vortex(&xVel, &yVel, vort, vortices, intermediateRadii, vortRadLen);

    switch (RKStep) {
        case 1: {
                    k1_x = xVel;
                    k1_y = yVel;

                    xVel = xVel/2.;
                    yVel = yVel/2.;
                    break;
                }
        case 2: {
                    k2_x = xVel;
                    k2_y = yVel;

                    xVel = xVel/2.;
                    yVel = yVel/2.;
                    break;
                }
        case 3: {
                    k3_x = xVel;
                    k3_y = yVel;
                    break;
                }
        case 4: {
                    k4_x = xVel;
                    k4_y = yVel;
                    break;
                }
        default: {
                     break;
                 }
    }

#ifdef DEBUG
    switch (RKStep) {
        case 1: {
                    printf("step: %i | vortex: %i | k1_x: %1.15f\n", currentTimestep, vort->vIndex, k1_x);
                    printf("step: %i | vortex: %i | k1_y: %1.15f\n", currentTimestep, vort->vIndex, k1_y);
                    break;
                }
        case 2: {
                    printf("step: %i | vortex: %i | k2_x: %1.15f\n", currentTimestep, vort->vIndex, k2_x);
                    printf("step: %i | vortex: %i | k2_y: %1.15f\n", currentTimestep, vort->vIndex, k2_y);
                    break;
                }
        case 3: {
                    printf("step: %i | vortex: %i | k3_x: %1.15f\n", currentTimestep, vort->vIndex, k3_x);
                    printf("step: %i | vortex: %i | k3_y: %1.15f\n", currentTimestep, vort->vIndex, k3_y);
                    break;
                }
        case 4: {
                    printf("step: %i | vortex: %i | k4_x: %1.15f\n", currentTimestep, vort->vIndex, k4_x);
                    printf("step: %i | vortex: %i | k4_y: %1.15f\n", currentTimestep, vort->vIndex, k4_y);
                    break;
                }
        default: {
                     break;
                 }
    }
#endif

    for (int j = 0; j < numDriverVorts; ++j) {
        if (originVortIndex == j) {
            continue;
        }

        //		bool __atomic_compare_exchange (type *ptr, type *expected, type *desired, bool weak, int success_memorder, int failure_memorder)

        long radiiIndex = calculateVortexRadiiIndex(vortIndex, vortices[j].vIndex);

        // if (vortIndex != vortices[j].otherVort) {
        // vort->otherVort++;
        // }

        // Whether we add or subtract d_dj from radii depends on which index is larger.

        char addVals = vort->vIndex > vortices[j].vIndex;

        double oldXRad, oldYRad, oldRadMag;

        __atomic_load(&workingRadii[radiiIndex + 1], &oldXRad, __ATOMIC_SEQ_CST);
        double newXRad;
        do {
            newXRad = addVals ? oldXRad + xVel*timestep : oldXRad - xVel*timestep;
        } while (!__atomic_compare_exchange(&workingRadii[radiiIndex + 1], &oldXRad, &newXRad, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));

        __atomic_load(&workingRadii[radiiIndex + 2], &oldYRad, __ATOMIC_SEQ_CST);
        double newYRad;
        do {
            newYRad = addVals ? oldYRad + xVel*timestep : oldYRad - xVel*timestep;
        } while (!__atomic_compare_exchange(&workingRadii[radiiIndex + 2], &oldYRad, &newYRad, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));

        __atomic_thread_fence(__ATOMIC_SEQ_CST); // not sure if this is neccessary

        __atomic_load(&workingRadii[radiiIndex], &oldRadMag, __ATOMIC_SEQ_CST);
        double newRadMag;
        do {
            __atomic_load(&workingRadii[radiiIndex + 1], &newXRad, __ATOMIC_SEQ_CST);
            __atomic_load(&workingRadii[radiiIndex + 2], &newYRad, __ATOMIC_SEQ_CST);
            newRadMag = sqrt(pow(newXRad, 2) + pow(newYRad, 2));
        } while (!__atomic_compare_exchange(&workingRadii[radiiIndex], &oldRadMag, &newRadMag, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));

    }

    for (int tracerI = 0; tracerI < numTracers; tracerI++) {
        long index = calculateTracerRadiiIndex(tracerI, originVortIndex);
        intermediateTracerRads[index + 1] += xVel*timestep;
        intermediateTracerRads[index + 2] += yVel*timestep;
        intermediateTracerRads[index] = sqrt(pow(intermediateTracerRads[index + 1], 2) + pow(intermediateTracerRads[index + 2], 2));
    }

    if (SAVE_RK_STEPS) {
        struct Vector pos1;
        pos1.x = vort->position[0] + k1_x * timestep;
        pos1.y = vort->position[1] + k1_y * timestep;

        struct Vector pos2;
        pos1.x = vort->position[0] + k2_x * timestep;
        pos1.y = vort->position[1] + k2_y * timestep;

        struct Vector pos3;
        pos1.x = vort->position[0] + k3_x * timestep;
        pos1.y = vort->position[1] + k3_y * timestep;

        struct Vector pos4;
        pos1.x = vort->position[0] + k4_x * timestep;
        pos1.y = vort->position[1] + k4_y * timestep;

        struct RKPositions positions;
        positions.vort = vort;
        positions.step1Pos = pos1;
        positions.step2Pos = pos2;
        positions.step3Pos = pos3;
        positions.step4Pos = pos4;

        intPositionCache[vort->vIndex] = positions;
    }

    vort->velocity[0] += (k1_x + k2_x * 2 + k3_x * 2 + k4_x)/6;
    vort->velocity[1] += (k1_y + k2_y * 2 + k3_y * 2 + k4_y)/6;
    free(arguments);
}

/**
  moves the simulation forward 1 timestep using runge-kutta 4th order. This function updates all vortex and tracer positions and velocities
  @note This function does not update the appropriate radius arrays. Call @c UpdateRadii_Pythagorean() to update those arrays.

  @param vortices The array of all of the vortices in the simulation
  @param vortRadii The array of doubles containing vortex <-> vortex distance information
  @param tracerRadii The array of doubles containing vortex <-> tracer distance information
  @param numTracers The numebr of tracers
  */
void stepForward_RK4(struct Vortex *vortices, double *vortRadii, double *tracerRadii, struct Tracer *tracers, int numTracers) {
    int sizeOfRadEntry = sizeof(double) * 3;
    long vortRadLen = (pow(numDriverVorts, 2)-numDriverVorts)/2;
    long vortRadSize = vortRadLen * sizeOfRadEntry;
    intPositionCache = malloc(sizeof(struct RKPositions) * numDriverVorts);

    /*
       workingRadii is updated after every vortex is updated in position once per timestep, and should not be directly used to compute vortex velocities
       intermediateRadii is updated after every runge-kutta step. At the end of the step, intermediateRadii is coppied into workingRadii
       intermediateRadii should be used to compute the velocities of each particle.
       */

    double *workingRadii = malloc(vortRadSize);
    memcpy(workingRadii, vortRadii, vortRadSize);
    pthread_mutex_t radMutex;
    pthread_mutex_init(&radMutex, NULL);
    double *intermediateRadii = malloc(vortRadSize);
    memcpy(intermediateRadii, vortRadii, vortRadSize);

    long tracerRadLen = numDriverVorts * numTracers; // number of tracer radii entries in the array (each entry is 3 doubles: magnitude, xcomponent, ycomponent)
    long tracerRadSize = tracerRadLen * sizeOfRadEntry; // length of the array in bytes
    double *intermediateTracerRads = malloc(tracerRadSize);
    memcpy(intermediateTracerRads, tracerRadii, tracerRadSize);

    // zero out the velocities for all the vortices and tracers
    for (int i = 0; i < numDriverVorts; i++) {
        struct Vortex *vort = &vortices[i];
        vort->velocity[0] = 0;
        vort->velocity[1] = 0;
    }

    for (int i = 0; i < numTracers; i++) {
        struct Tracer *tracer = &tracers[i];
        tracer->velocity[0] = 0;
        tracer->velocity[1] = 0;
    }

    for (int RKStep = 1; RKStep <= 4; RKStep++) {
        //		double xVel = 0, yVel = 0;

        // tracer multithreading
        int tracersPerThread = NUM_TRACERS/THREADCOUNT;
        if (THREADCOUNT > 1) {
            for (int thread = 0; thread < THREADCOUNT; thread++) {
                struct Tracer *tracerArrayPiece = &tracers[thread * tracersPerThread];
                long radPieceStartIndex = calculateTracerRadiiIndex(tracerArrayPiece[0].tIndex, 0);
                double *tracerRadArrayPiece = &tracerRadii[radPieceStartIndex];
                double *intermediateTracerRadArrayPiece = &intermediateTracerRads[radPieceStartIndex];

                if (thread == THREADCOUNT - 1) {
                    // if the tracers couldn't be evenly divided between threads, then we just put the extra ones on the last thread.
                    // TODO: do a better job of splitting this up. In the worst case, the last thread could have 2*(tracersPerThread)-1 tracers

                    tracersPerThread += NUM_TRACERS%THREADCOUNT;
                }

                // you can only pass 1 parameter to a function being run by another thread. To get around this, you create a struct with all the information
                // you want to pass, then just pass a pointer to that struct as the 1 parameter to the function. 
                struct TracerArgs *args = malloc(sizeof(struct TracerArgs));
                args->RKStep = RKStep;
                args->tracers = tracerArrayPiece;
                args->numTracers = tracersPerThread;
                args->tracerRadii = tracerRadArrayPiece;
                args->intermediateTracerRads = intermediateTracerRadArrayPiece;
                args->vortices = vortices;

                thpool_add_work(thpool, stepForwardTracerRK4, args);
            }

            thpool_wait(thpool);

        } else { // just run on the main thread to make debugging easier if there's only 1 thread
            struct TracerArgs *args = malloc(sizeof(struct TracerArgs));
            args->RKStep = RKStep;
            args->tracers = tracers;
            args->numTracers = tracersPerThread;
            args->tracerRadii = tracerRadii;
            args->intermediateTracerRads = intermediateTracerRads;
            args->vortices = vortices;

            stepForwardTracerRK4(args);
        }

        /********* end of tracer step code **************/

        for (int originVortIndex = 0; originVortIndex < numDriverVorts; originVortIndex++) {
            struct VortexArgs *args = malloc(sizeof(struct VortexArgs));
            args->RKStep = RKStep;
            args->vortices = vortices;
            args->originVortIndex = originVortIndex;
            args->intermediateRadii = intermediateRadii;
            args->workingRadii = workingRadii;
            args->vortRadLen = vortRadLen;
            args->intermediateTracerRads = intermediateTracerRads;
            args->numTracers = NUM_TRACERS;

            thpool_add_work(thpool, stepForwardVortexRK4, args);
        }

        thpool_wait(thpool);

        memcpy(intermediateRadii, workingRadii, vortRadSize);
        memcpy(workingRadii, vortRadii, vortRadSize);
    }

    for (int i = 0; i < numDriverVorts; ++i) {
        struct Vortex *vort = &vortices[i];
        vort->position[0] += vort->velocity[0] * timestep;
        vort->position[1] += vort->velocity[1] * timestep;
    }

    for (int i = 0; i < numTracers; ++i) {
        struct Tracer *tracer = &tracers[i];
        tracer->position[0] += tracer->velocity[0] * timestep;
        tracer->position[1] += tracer->velocity[1] * timestep;
    }

    memcpy(tracerRadii, intermediateTracerRads, tracerRadSize);

    free(workingRadii);
    free(intermediateRadii);
    free(intermediateTracerRads);
    pthread_mutex_destroy(&radMutex);

    saveIntermediateVortPositions(numDriverVorts, intPositionCache);

    free(intPositionCache);
}

#pragma mark - Vortex Lifecycle

/**
  delete a vortex and all associated data from the simulation

  @param vort the vortex to remove
  @param vorts the array of vortices
  @param tracerRads the array of doubles containing all tracer <-> vortex radius data
  */
void deleteVortex(struct Vortex *vort, double *vortexRads, struct Vortex *vorts, double *tracerRads) {
    int deletionIndex = vort->vIndex;

    // remove vortex from vortexRadii array

    double *destPtr;
    double *sourcePtr;
    for (int rowIndex = deletionIndex; rowIndex < numDriverVorts - 1; rowIndex++) {
        // shift radii up a row if they are before the column being deleted
        destPtr = &vortexRads[calculateVortexRadiiIndex(0, rowIndex)];
        sourcePtr = &vortexRads[calculateVortexRadiiIndex(0, rowIndex+1)];
        memmove(destPtr, sourcePtr, deletionIndex * sizeof(double) * 3);

        if (rowIndex == numDriverVorts-2)
            // shift radii up and left if they are after the column being deleted
            destPtr = &vortexRads[calculateVortexRadiiIndex(deletionIndex, rowIndex)];
        sourcePtr = &vortexRads[calculateVortexRadiiIndex(deletionIndex + 1, rowIndex+1)];
        memmove(destPtr, sourcePtr, (rowIndex - deletionIndex) * sizeof(double) * 3);
    }

    // remove vortex's radius data from the tracer radii array
    for (int tracerI = 0; tracerI < NUM_TRACERS; tracerI++) {
        long tRadDelIndex = calculateTracerRadiiIndex(tracerI, deletionIndex);

        memmove(&tracerRads[tRadDelIndex], &tracerRads[tRadDelIndex+1], (numDriverVorts-deletionIndex) * sizeof(double) * 3);
    }

    // remove vortex from vorts array
    free(vort->position);
    free(vort->velocity);
    memmove(&vorts[deletionIndex], &vorts[deletionIndex+1], sizeof(struct Vortex) * (numDriverVorts-deletionIndex-1));

    numDriverVorts--; // has to happen exactly here to avoid OB1 errors

    // fix other vorts' vIndex values
    for (int i = deletionIndex; i < numDriverVorts; i++) vorts[i].vIndex--;
}

int nextVortID = 0;
/**
 Randomize the position and intensity of a vortex, and also give it a new vID. This is the same as deleting the vortex
 then initializing a new one, however it doesn't require an O(n) (where n is the total number of vortices being simulated)
 deletion from the array of vortices.
 */
void randomizeVortex(struct Vortex *vort) {
    vort->vID = nextVortID++;
    vort->position[0] = generateUniformRandInRange(0, DOMAIN_SIZE_X);
    vort->position[1] = generateUniformRandInRange(0, DOMAIN_SIZE_Y);

    vort->intensity = generateNormalRand(VORTEX_INTENSITY_SIGMA);
    double minIntensity = .001;

    do {
        vort->intensity = generateNormalRand(VORTEX_INTENSITY_SIGMA);
    } while (fabs(vort->intensity) < minIntensity);
    vort->velocity[0] = 0;
    vort->velocity[1] = 0;
    vort->initStep = currentTimestep;
}


/**
  compute the signed square root of the absolute value of the sum of the signed squared intensities // figure out how to site Mark's 2016
  */
double mergeIntensities(double int1, double int2) {
    char sign1 = int1/fabs(int1);
    char sign2 = int2/fabs(int2);

    double newInt = sqrt(fabs(sign1 * pow(int1, 2) + sign2 * pow(int2, 2)));
    return (int1 + int2 > 0) ? newInt : -newInt;
}

/**
  @note this will increment numDriverVorts. It is not neccessary to incrememnt numDriverVorts before/after this function runs
  */
void spawnVortex(struct Vortex *vorts) {
    int spawnIndex = numDriverVorts++;

    struct Vortex *vort = &vorts[spawnIndex];
    vort->vIndex = spawnIndex;
    vort->position = malloc(sizeof(double) * 2);
    vort->velocity = calloc(sizeof(double), 2);

    randomizeVortex(vort); // creates random position/intensity
}

void spawnVorts(double **tracerRads, struct Vortex **vorts, double **vortexRadii, int *vortsAllocated, int numVortsToSpawn) {
    int spawnsLeft = numVortsToSpawn;

    if (numDriverVorts + spawnsLeft >= *vortsAllocated) {
        *vortsAllocated = (numDriverVorts + spawnsLeft) * 1.5;
        *vorts = realloc(*vorts, *vortsAllocated * sizeof(struct Vortex));

        long newVortRadiiLen = (*vortsAllocated * (*vortsAllocated-1))/2; // # of edges in a complete graph of vortsAllocated nodes
        long newVortRadiiSize = newVortRadiiLen * sizeof(double) * 3;
        *vortexRadii = realloc(*vortexRadii, newVortRadiiSize);

        long newTracerRadiiLen = *vortsAllocated * NUM_TRACERS;
        long newTracerRadiiSize = newTracerRadiiLen * sizeof(double) * 3;
        *tracerRads = realloc(*tracerRads, newTracerRadiiSize);

        if (*vorts == NULL) {
            printf("Error reallocating vorts array");
            exit(1);
        } else if (*vorts == NULL) {
            printf("Error reallocating vorts array");
            exit(1);
        } else if (*vorts == NULL) {
            printf("Error reallocating vorts array");
            exit(1);
        }
    }

    while (spawnsLeft--) {
        spawnVortex(*vorts);
    }
}

/**
  @discussion Find and merge all vortices which are within VORTEX_MERGE_RADIUS of eachother. If spawning is going will happen after this, then
  this function can re-initialize merged vortices rather than doing a niave deletion. This saves having to do lots of memmove's to rearrange
  the radii arrays.

  @param spawnsLeft This will spawn no more than spawnsLeft number of vortices. If > spawnsLeft merges happen, then the extra vortices are simply deleted.

  @return The remaining number of spawns after merging is complete.
  */
int mergeVorts(double *vortexRadii, struct Vortex *vorts, double *tracerRads, struct Tracer *tracers, int spawnsLeft, int *totalMerges) {
    int merges;
    do {
        merges = 0;

        for (int vortIndex2 = 1; vortIndex2 < numDriverVorts; vortIndex2++) {
            for (int vortIndex1 = 0; vortIndex1 < vortIndex2; vortIndex1++) {
                long radIndex = calculateVortexRadiiIndex(vortIndex1, vortIndex2);
                if (vortexRadii[radIndex] < VORTEX_MERGE_RADIUS) {
                    merges++;

                    struct Vortex *vort1 = &vorts[vortIndex1];
                    struct Vortex *vort2 = &vorts[vortIndex2];

                    if (totalMerges) (*totalMerges)++;

                    double absInt1 = fabs(vort1->intensity);
                    double absInt2 = fabs(vort2->intensity);

                    // compute new position and vorticity
                    double newXPos = (vort1->position[0]*absInt1 + vort2->position[0]*absInt2) / (absInt1 + absInt2);
                    double newYPos = (vort1->position[1]*absInt1 + vort2->position[1]*absInt2) / (absInt1 + absInt2);
                    double newIntensity = mergeIntensities(vort1->intensity, vort2->intensity);

                    vort1->position[0] = newXPos;
                    vort1->position[1] = newYPos;
                    vort1->intensity = newIntensity;

                    // i think that deleting vort2 is actually slower than deleting vort1, but the difference should be fairly insignificant
                    if (spawnsLeft) {
                        spawnsLeft--;
                        randomizeVortex(vort2);
                    } else {
                        deleteVortex(vort2, vortexRadii, vorts, tracerRads); // a faster way to do this would be to mark each vortex for deletion, then go through and remove them all at once
                    }
                    updateRadii_pythagorean(vortexRadii, vorts, tracerRads, tracers, NUM_TRACERS); // if this becomes a significant speed issue, a new function which only computes the relevant radii should be written
                    break;
                }
            }
        }
    } while (merges > 0);

    return spawnsLeft;
}

#pragma mark - Initializers

/**
  generate the tracers

  @discussion this function does not put any tracers on the edge of the domain. If the space between tracers at t=0 is 1, there is a gap of size 1
  between each edge of the center domain and the closest column/row of tracers.

  @param tracers the array to fill with tracers
  @param n the number of tracers to generate
  */
void initialize_tracers(struct Tracer *tracers, int n) {
    double seperationX = (DOMAIN_SIZE_X) / (sqrt(n) + 1);
    double seperationY = (DOMAIN_SIZE_Y) / (sqrt(n) + 1);

    assert(sqrt(n) == (int)sqrt(n));

    int i = 0;

    for (int row = 1; row <= sqrt(n); row++) {
        for (int col = 1; col <= sqrt(n); col++) {
            tracers[i].tIndex = i;
            tracers[i].position = malloc(sizeof(double) * 2);
            tracers[i].velocity = calloc(sizeof(double), 2);

            tracers[i].position[0] = col * seperationX;
            tracers[i].position[1] = row * seperationY;
            i++;
        }
    }
}

void initialize_single_test_tracer(struct Tracer *tracers, int numTracers, struct Vortex *vorts) {
    assert(numTracers == 1);

    initialize_tracers(tracers, numTracers);
    memcpy(tracers[0].position, vorts[0].position, sizeof(double) * 2);
}

#pragma mark - misc

/**
  find the smallest radius seperation between vortices

  @param radArr the array of doubles contianing all vortex radius information
  @param numVorts the number of active driver vortices
  */
double minRad(double *radArr, long numVorts) {
    if (numVorts == 0) return 0;
    long radLen = (pow(numVorts, 2)-numVorts)/2 * 3;

    double min = radArr[0];
    for (int i = 0; i < radLen; i += 3) {
        if (radArr[i] < min) min = radArr[i];
    }
    return min;
}

/**
  print the vortRads array in a human readable format
  */
void pprintVortRads(double *rads, int numActiveVorts) {
    long index = 0;
    for (int row = 0; row < numActiveVorts; row++) {
        for (int col = 0; col < row; col++) {
            printf("|");
            for (int i = 0; i < 3; i++) {
                printf("%6.2f", rads[index]);
                if (i != 2) printf(",");
                index++;
            }
            if (col == row-1) printf("|");
        }
        printf("\n");
    }
}
/** print the tracerRads array in a human readable format*/
void pprintTracerRads(double *rads, int numActiveTracers) {
    long index = 0;
    for (int vortIndex = 0; vortIndex < numDriverVorts; vortIndex++) {
        for (int tracerIndex = 0; tracerIndex < numActiveTracers; tracerIndex++) {
            printf("|");
            for (int i = 0; i < 3; i++) {
                printf("%6.2f", rads[index]);
                if (i != 2) printf(",");
                index++;
            }
        }
        printf("|\n");
    }
}
/**find vortices/tracers which have moved beyond the edges of the driver domain, and move them to the opposite side of the array.*/
void wrapPositions(struct Vortex *vorts, struct Tracer *tracers, int numTracers) {
    for (int i = 0; i < numDriverVorts; i++) {
        if (vorts[i].position[0] < 0) {
            vorts[i].position[0] = DOMAIN_SIZE_X + fmod(vorts[i].position[0], DOMAIN_SIZE_X);
        } else if (vorts[i].position[0] > DOMAIN_SIZE_X) {
            vorts[i].position[0] =  fmod(vorts[i].position[0], DOMAIN_SIZE_X);
        }
        if (vorts[i].position[1] < 0) {
            vorts[i].position[1] = DOMAIN_SIZE_Y + fmod(vorts[i].position[1], DOMAIN_SIZE_Y);
        } else if (vorts[i].position[1] > DOMAIN_SIZE_Y) {
            vorts[i].position[1] = fmod(vorts[i].position[1], DOMAIN_SIZE_Y);
        }
    }

    for (int i = 0; i < numTracers; i++) {
        if (tracers[i].position[0] < 0) {
            tracers[i].position[0] = DOMAIN_SIZE_X + fmod(tracers[i].position[0], DOMAIN_SIZE_X);;
        } else if (tracers[i].position[0] > DOMAIN_SIZE_X) {
            tracers[i].position[0] = fmod(tracers[i].position[0], DOMAIN_SIZE_X);
        }
        if (tracers[i].position[1] < 0) {
            tracers[i].position[1] = DOMAIN_SIZE_Y + fmod(tracers[i].position[1], DOMAIN_SIZE_Y);
        } else if (tracers[i].position[1] > DOMAIN_SIZE_Y) {
            tracers[i].position[1] = fmod(tracers[i].position[1], DOMAIN_SIZE_Y);
        }
    }
}
/**
  find the highest velocity vortex

  @return the magnitude of the largest velocity vector
  */
double maxVelocity(struct Vortex *vorts) {
    double maxV = 0;
    for (int i = 0; i < numDriverVorts; i++) {
        struct Vortex *vort = &vorts[i];
        double totalV = sqrt(pow(vort->velocity[0], 2) + pow(vort->velocity[1], 2));
        if (totalV > maxV) maxV = totalV;
    }
    return maxV;
}

double carryoverSpawnCount;
int vortsSpawned = 0;
/**
  calculate the numebr of vorties to spawn in the next time step
  */
int calcSpawnCount() {
    if ((0)) {
        if (currentTimestep == 0) carryoverSpawnCount = 0;
        // calculate the number of vortices to spawn using Mark's scheme

        double spawnCount = carryoverSpawnCount + VORTEX_SPAWN_RATE * timestep;
        if (spawnCount > 1.) {
            carryoverSpawnCount = fmod(spawnCount, 1.);
            if (carryoverSpawnCount < .000001) carryoverSpawnCount = 0;
            vortsSpawned += spawnCount;
            spawnCount = (int)floor(spawnCount - carryoverSpawnCount); // I think that this should round instead of truncating
        } else {
            carryoverSpawnCount = spawnCount;
        }
        return (int)spawnCount;
    } else {
        // calculate the number of vortices to spawn using a poisson PDF
        return generatePoissonRand(0, VORTEX_SPAWN_RATE, 0);
    }
}

/**
  handle SIGTERMs
  */
void termination_handler(int sig) {
#ifdef SAVE_RAWDATA
    closeFile();
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}

void initializeSimulation(struct Vortex *vortices[], int *numDriverVorts, double *vortexRadii[], struct Tracer *tracers[], double *tracerRadii[], int *vorticesAllocated) {
    // setup sigterm handlers
    signal(SIGTERM, termination_handler);
    signal(SIGINT, termination_handler);
    // initialize the threadpool
    if (THREADCOUNT > 1) {
        thpool = thpool_init(THREADCOUNT);
    }

    // if INITFNAME isn't an empty string, INIT_TIME_STEP isn't negative, and TEST_CASE is 0
    // then we initialie the simulation from the file given by INITFNAME, starting at the 
    // timestep given by INIT_TIME_STEP
    if (INITFNAME[0] && INIT_TIME_STEP >= 0 && TEST_CASE == 0) {
        initFromFile(INITFNAME, INIT_TIME_STEP, vortices, numDriverVorts, vorticesAllocated, tracers);
    }
    // seed the RNG
    if (FIRST_SEED == -1) {
        currentTimestep = 0;
        time((time_t *)&lastX);

        printf("First random seed is %li\n", lastX);
    } else {
        lastX = FIRST_SEED;
    }

    numDriverVorts = 0;
    *vorticesAllocated = (int)NUM_VORT_INIT*1.5;
    // vortices is the array of Vortex structs
    *vortices = malloc(sizeof(struct Vortex) * *vorticesAllocated);

    // tracers is the array of Tracer structs
    *tracers = malloc(sizeof(struct Tracer) * NUM_TRACERS);

    if (TEST_CASE == 0) {
        int spawnsRemaining = NUM_VORT_INIT;
        spawnVorts(tracerRadii, vortices, vortexRadii, vorticesAllocated, spawnsRemaining);
        initialize_tracers(*tracers, NUM_TRACERS);
        // Generate vortices so that they don't end up being merged on the first timestep.
    } else {
        spawnVorts(tracerRadii, vortices, vortexRadii, vorticesAllocated, NUM_VORT_INIT);
        initialize_test(*vortices, *numDriverVorts);
        if (TEST_CASE != 6) {
            initialize_tracers(*tracers, NUM_TRACERS);
        } else {
            initialize_single_test_tracer(*tracers, NUM_TRACERS, *vortices);
        }
    }

    unsigned long vortexRadiiSize = sizeof(double) * (calculateVortexRadiiIndex(*vorticesAllocated-1, *vorticesAllocated-2) + 3);

    // vortexRadii is the matrix of distances between vortices. The distance between vortex vIndex==a and vortex vIndex==b (where a < b) is at index 3*(a*(a+1)/2+b).
    // the next item in the array is the x-component of the distance, and then the y-component of the distance
    // r, r_x, r_y

    *vortexRadii = malloc(vortexRadiiSize);

    long tracerRadSize = NUM_TRACERS * *vorticesAllocated * sizeof(double) * 3;
    *tracerRadii = malloc(tracerRadSize); // row = vortex, col = tracer; Note: vortPos - tracerPos

    updateRadii_pythagorean(*vortexRadii, *vortices, *tracerRadii, *tracers, NUM_TRACERS);
    
    // set nextVortID to be one more than the highest vortex ID.
    for (int i = 0; i < *numDriverVorts; i++) {
        if ((*vortices[i]).vID >= nextVortID) {
            nextVortID = (*vortices[i]).vID;
        }
    }
}

#pragma mark - Main

int main(int argc, const char * argv[]) {
    struct Vortex *vortices;
    struct Tracer *tracers;
    double *vortexRadii;
    double *tracerRadii;
    int vorticesAllocated;

    // load values for constants from the config file
    importConstants("./config"); 
    // initialize the vortices and drivers. Either write zeros into the arrays, or read data from the
    // input file into the simulation. 
    initializeSimulation(&vortices, &numDriverVorts, &vortexRadii, &tracers, &tracerRadii, &vorticesAllocated);

    struct timespec initFinishedTime;
    clock_gettime(CLOCK_MONOTONIC, &initFinishedTime);

    // only used to verify that the simulation matches the analytic solution to test case #4
    // not really useful for anything else
    double currentTime;
    if (TEST_CASE == 4) currentTime = 0;

    /******************* main loop *******************/

    while (NUMBER_OF_STEPS == 0 || currentTimestep < NUMBER_OF_STEPS) {
        struct timespec startTime;
        struct timespec endTime;

        // if DRAW_CONSOLE is true, then the simulation draws an asii representation of the vortices
        // and tracers to the console. This is sometimes useful for debugging.
        if (DRAW_CONSOLE) {
            if (currentTimestep%RENDER_NTH_STEP == 0) {
                drawToConsole(vortices, numDriverVorts, tracers);
                for (int c = 0; c < ceil(log10(currentTimestep)) + 1; c++) printf("\010");
                printf("%i", currentTimestep);
                struct timespec sleepDuration;
                sleepDuration.tv_sec = 0;
                sleepDuration.tv_nsec = (1./5)*1E9;

                nanosleep(&sleepDuration, NULL);
            }
        }

        // if DRAW_PNG is true then we save pdf images (which can be assembled into video)
        if (DRAW_PNG) {
            if (currentTimestep%RENDER_NTH_STEP == 0) {
                char *filename = malloc(sizeof(char) * 50);
                genFName(filename, currentTimestep);

                clock_gettime(CLOCK_MONOTONIC, &startTime);
                drawToFile(vortices, numDriverVorts, tracers, filename);
                free(filename);
                clock_gettime(CLOCK_MONOTONIC, &endTime);
            }
        }

        // generate parameters used to verify that this simulator matches the analytic solution for test case 4
        if (TEST_CASE == 4) {
            double minR = minRad(vortexRadii, numDriverVorts);
            double maxV = maxVelocity(vortices);
            timestep = minR / maxV * .5;
            if (timestep > TIMESTEP_CONST || maxV == 0) timestep = TIMESTEP_CONST;

            currentTime += timestep;
            if (currentTime > 50) return 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &startTime);

        // merge and spawn vortices
        if (VORTEX_LIFECYCLE) {
            int numSpawns = calcSpawnCount();
            printf("spawning %i vorts\n", numSpawns);
            int totalMergeCount = 0;
            int spawnsLeft = mergeVorts(vortexRadii, vortices, tracerRadii, tracers, numSpawns, &totalMergeCount);
            spawnVorts(&tracerRadii, &vortices, &vortexRadii, &vorticesAllocated, spawnsLeft);
            updateRadii_pythagorean(vortexRadii, vortices, tracerRadii, tracers, NUM_TRACERS);
            mergeVorts(vortexRadii, vortices, tracerRadii, tracers, 0, &totalMergeCount);
            printf("timestep: %i, time: %.5f, totMerges: %i\n", currentTimestep, currentTimestep * timestep, totalMergeCount);
        }

        // compute the new positions of tracers and vortices using runge-kutta 4th order
        stepForward_RK4(vortices, vortexRadii, tracerRadii, tracers, NUM_TRACERS);
        // find vortices which have moved out of the domain, then move them to their new positions
        wrapPositions(vortices, tracers, NUM_TRACERS);
        // after warping, the radii arrays may not be correct, so I recalculate them. Since this only happens once per
        // time step, this computation takes a negligable amount of time.
        updateRadii_pythagorean(vortexRadii, vortices, tracerRadii, tracers, NUM_TRACERS);

        clock_gettime(CLOCK_MONOTONIC, &endTime);
        double sec = (endTime.tv_sec - startTime.tv_sec) + (double)(endTime.tv_nsec - startTime.tv_nsec) / 1E9;
        printf("Step number %i calculation complete in %f sec with %i vortices\n", currentTimestep, sec, numDriverVorts);

        // if SAVE_RAWDATA, then we save the position once per timestep
        if (SAVE_RAWDATA) {
            if (currentTimestep == 0) openFile();
            saveState(currentTimestep, lastX, numDriverVorts, NUM_TRACERS, vortices,tracers);
        }

        fflush(stdout);
        currentTimestep++;
    }

    struct timespec simFinishedTime;
    clock_gettime(CLOCK_MONOTONIC, &simFinishedTime);
    double sec = (simFinishedTime.tv_sec - initFinishedTime.tv_sec) + (double)(simFinishedTime.tv_nsec - initFinishedTime.tv_nsec) / 1E9;
    printf("Total simulation runtime: %f\n", sec);

    for (int vortIndex = 0; vortIndex < numDriverVorts; vortIndex++) {
        free(vortices[vortIndex].position);
        free(vortices[vortIndex].velocity);
    }

    for (int tracerIndex = 0; tracerIndex < NUM_TRACERS; tracerIndex++) {
        free(tracers[tracerIndex].position);
        free(tracers[tracerIndex].velocity);
    }

    free(vortices);
    free(tracers);
    free(vortexRadii);
    free(tracerRadii);
    thpool_destroy(thpool);

#ifdef SAVE_RAWDATA
    closeFile();
#endif

    return 0;
}
