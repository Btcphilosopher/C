VR Monorail Simulator



Here is a standalone C11 prototype for the train/track physics and driver controls:

/*
    VR MONORAIL SIMULATOR
    C11 prototype

    Compile:
        cc -std=c11 -O2 vr_monorail.c -lm -o vr_monorail
*/

#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#define MAX_STATIONS 16
#define MAX_TRACK_POINTS 256

typedef struct {
    double x;
    double y;
    double z;
} Vec3;

typedef struct {
    char name[64];

    double distance;
    double platform_length;

    bool terminal;
} Station;

typedef struct {
    Vec3 position;

    double distance;
    double gradient;
    double curvature;

    double speed_limit;
} TrackPoint;

typedef struct {
    double throttle;
    double brake;

    bool emergency_brake;
    bool door_open;
    bool horn;
} DriverControls;

typedef struct {
    double mass;

    double position;
    double velocity;

    double acceleration;

    double max_speed;

    double traction_force;
    double brake_force;

    double rolling_resistance;

    bool doors_open;
} Monorail;

typedef struct {

    TrackPoint points[MAX_TRACK_POINTS];
    int point_count;

    Station stations[MAX_STATIONS];
    int station_count;

} Track;

typedef struct {

    Monorail train;
    DriverControls controls;
    Track track;

    double time;

} Simulator;


/* --------------------------------------------------------- */
/* Utility */
/* --------------------------------------------------------- */

static double clamp(double x, double min, double max)
{
    if (x < min) return min;
    if (x > max) return max;
    return x;
}


/* --------------------------------------------------------- */
/* Track creation */
/* --------------------------------------------------------- */

static void track_init(Track *track)
{
    memset(track, 0, sizeof(*track));
}

static void track_add_point(
    Track *track,
    double distance,
    double gradient,
    double curvature,
    double speed_limit)
{
    if (track->point_count >= MAX_TRACK_POINTS)
        return;

    TrackPoint *p =
        &track->points[track->point_count++];

    p->distance = distance;
    p->gradient = gradient;
    p->curvature = curvature;
    p->speed_limit = speed_limit;

    p->position.x = distance;
    p->position.y = 0.0;
    p->position.z = 0.0;
}

static void track_add_station(
    Track *track,
    const char *name,
    double distance,
    double platform_length,
    bool terminal)
{
    if (track->station_count >= MAX_STATIONS)
        return;

    Station *s =
        &track->stations[track->station_count++];

    snprintf(
        s->name,
        sizeof(s->name),
        "%s",
        name
    );

    s->distance = distance;
    s->platform_length = platform_length;
    s->terminal = terminal;
}


/* --------------------------------------------------------- */
/* Find current track section */
/* --------------------------------------------------------- */

static TrackPoint track_sample(
    const Track *track,
    double distance)
{
    TrackPoint result = track->points[0];

    for (int i = 0; i < track->point_count - 1; i++) {

        TrackPoint a = track->points[i];
        TrackPoint b = track->points[i + 1];

        if (distance >= a.distance &&
            distance <= b.distance)
        {
            double length =
                b.distance - a.distance;

            double t =
                (distance - a.distance) / length;

            result.distance = distance;

            result.gradient =
                a.gradient +
                (b.gradient - a.gradient) * t;

            result.curvature =
                a.curvature +
                (b.curvature - a.curvature) * t;

            result.speed_limit =
                a.speed_limit +
                (b.speed_limit - a.speed_limit) * t;

            return result;
        }
    }

    return result;
}


/* --------------------------------------------------------- */
/* Train physics */
/* --------------------------------------------------------- */

static void monorail_update(
    Monorail *train,
    const DriverControls *controls,
    TrackPoint track,
    double dt)
{
    const double gravity = 9.81;

    /*
        Maximum traction force.
    */

    const double max_traction = 180000.0;

    /*
        Service braking force.
    */

    const double max_brake = 220000.0;

    /*
        Emergency braking.
    */

    const double emergency_brake = 420000.0;

    /*
        Traction.
    */

    double traction =
        controls->throttle * max_traction;

    /*
        Braking.
    */

    double braking =
        controls->brake * max_brake;

    if (controls->emergency_brake)
        braking = emergency_brake;

    /*
        Rolling resistance.
    */

    double rolling =
        train->rolling_resistance;

    /*
        Gravity on gradient.
    */

    double gravity_force =
        train->mass *
        gravity *
        track.gradient;

    /*
        Net longitudinal force.
    */

    double force =
        traction
        - braking
        - rolling
        - gravity_force;

    /*
        Newton's second law.
    */

    train->acceleration =
        force / train->mass;

    /*
        Integrate velocity.
    */

    train->velocity +=
        train->acceleration * dt;

    /*
        Prevent reverse movement.
    */

    if (train->velocity < 0.0)
        train->velocity = 0.0;

    /*
        Track speed limit.
    */

    if (train->velocity >
        track.speed_limit)
    {
        train->velocity =
            track.speed_limit;
    }

    /*
        Integrate position.
    */

    train->position +=
        train->velocity * dt;

    /*
        Doors.
    */

    train->doors_open =
        controls->door_open;
}


/* --------------------------------------------------------- */
/* Station detection */
/* --------------------------------------------------------- */

static int current_station(
    const Track *track,
    double position)
{
    for (int i = 0;
         i < track->station_count;
         i++)
    {
        double distance =
            fabs(position -
                 track->stations[i].distance);

        if (distance < 5.0)
            return i;
    }

    return -1;
}


/* --------------------------------------------------------- */
/* Haptic feedback model */
/* --------------------------------------------------------- */

static double haptic_intensity(
    const Monorail *train)
{
    /*
        More acceleration =
        stronger controller vibration.
    */

    double acceleration =
        fabs(train->acceleration);

    double vibration =
        acceleration / 3.0;

    return clamp(
        vibration,
        0.0,
        1.0
    );
}


/* --------------------------------------------------------- */
/* Simulator */
/* --------------------------------------------------------- */

static void simulator_init(
    Simulator *sim)
{
    memset(sim, 0, sizeof(*sim));

    sim->train.mass =
        65000.0;

    sim->train.max_speed =
        33.33; /* 120 km/h */

    sim->train.rolling_resistance =
        1500.0;

    /*
        Example monorail route.
    */

    track_init(&sim->track);

    track_add_point(
        &sim->track,
        0.0,
        0.0,
        0.0,
        22.22
    );

    track_add_point(
        &sim->track,
        500.0,
        0.01,
        0.0,
        27.78
    );

    track_add_point(
        &sim->track,
        1000.0,
        0.0,
        0.002,
        33.33
    );

    track_add_point(
        &sim->track,
        2000.0,
        -0.01,
        0.001,
        33.33
    );

    track_add_point(
        &sim->track,
        3000.0,
        0.0,
        0.0,
        22.22
    );

    track_add_point(
        &sim->track,
        4000.0,
        0.0,
        0.0,
        22.22
    );

    track_add_station(
        &sim->track,
        "Victoria",
        0.0,
        120.0,
        false
    );

    track_add_station(
        &sim->track,
        "Central",
        1500.0,
        120.0,
        false
    );

    track_add_station(
        &sim->track,
        "Airport",
        3000.0,
        150.0,
        false
    );

    track_add_station(
        &sim->track,
        "Harbour",
        4000.0,
        120.0,
        true
    );
}


/* --------------------------------------------------------- */
/* Simulation step */
/* --------------------------------------------------------- */

static void simulator_update(
    Simulator *sim,
    double dt)
{
    TrackPoint track =
        track_sample(
            &sim->track,
            sim->train.position
        );

    monorail_update(
        &sim->train,
        &sim->controls,
        track,
        dt
    );

    sim->time += dt;
}


/* --------------------------------------------------------- */
/* Main */
/* --------------------------------------------------------- */

int main(void)
{
    Simulator sim;

    simulator_init(&sim);

    /*
        Start driving.
    */

    sim.controls.throttle = 0.85;

    const double dt = 0.02;

    for (int frame = 0;
         frame < 1000;
         frame++)
    {
        /*
            Simulate braking near
            the Central station.
        */

        if (sim.train.position > 1300.0)
        {
            sim.controls.throttle = 0.0;
            sim.controls.brake = 0.65;
        }

        simulator_update(
            &sim,
            dt
        );

        if (frame % 25 == 0)
        {
            TrackPoint track =
                track_sample(
                    &sim.track,
                    sim.train.position
                );

            int station =
                current_station(
                    &sim.track,
                    sim.train.position
                );

            printf(
                "t=%6.2f  "
                "pos=%7.1fm  "
                "speed=%6.2f m/s  "
                "acc=%6.2f m/s2  "
                "limit=%6.2f m/s",
                sim.time,
                sim.train.position,
                sim.train.velocity,
                sim.train.acceleration,
                track.speed_limit
            );

            if (station >= 0)
            {
                printf(
                    "  STATION=%s",
                    sim.track.stations[station].name
                );
            }

            printf(
                "  haptic=%0.2f\n",
                haptic_intensity(
                    &sim.train
                )
            );
        }
    }

    return 0;
}






/*
    2030s URBAN MONORAIL SIMULATOR
    --------------------------------

    C11 railway simulation core.

    Features:
      - Elevated monorail
      - Multiple stations
      - Passenger demand
      - Automatic train operation
      - Traction
      - Service braking
      - Regenerative braking
      - Battery / energy model
      - Speed restrictions
      - Timetable
      - Station dwell times
      - AI trains
      - Weather
      - Railway signalling
      - Delays

    Build:

      cc -std=c11 -O2 monorail2030.c -lm -o monorail2030
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#define MAX_STATIONS 16
#define MAX_TRAINS   12
#define MAX_BLOCKS   32

#define DT 0.1

#define GRAVITY 9.81

/* ---------------------------------------------------------
   Basic types
   --------------------------------------------------------- */

typedef struct {
    double x;
    double y;
    double z;
} Vec3;


/* ---------------------------------------------------------
   Weather
   --------------------------------------------------------- */

typedef enum {
    WEATHER_CLEAR,
    WEATHER_RAIN,
    WEATHER_FOG,
    WEATHER_SNOW,
    WEATHER_STORM
} WeatherType;

typedef struct {

    WeatherType type;

    double temperature;
    double wind_speed;
    double visibility;

} Weather;


/* ---------------------------------------------------------
   Stations
   --------------------------------------------------------- */

typedef struct {

    char name[64];

    double position;

    double platform_length;

    int passenger_demand;

    int passengers_waiting;

    bool interchange;

} Station;


/* ---------------------------------------------------------
   Track
   --------------------------------------------------------- */

typedef struct {

    double start;
    double end;

    double speed_limit;

    double gradient;

    int block;

} TrackSection;


/* ---------------------------------------------------------
   Railway blocks
   --------------------------------------------------------- */

typedef struct {

    bool occupied;

    int train_id;

} SignalBlock;


/* ---------------------------------------------------------
   Train
   --------------------------------------------------------- */

typedef struct {

    int id;

    char name[32];

    double position;

    double velocity;

    double acceleration;

    double target_speed;

    double length;

    double mass;

    double passenger_mass;

    int passengers;

    int capacity;

    double traction_power;

    double braking_power;

    double battery_kwh;

    double battery_capacity;

    double regenerative_energy;

    bool doors_open;

    bool at_station;

    bool moving;

    bool automatic;

    double dwell_timer;

    double delay;

    int current_station;

} Train;


/* ---------------------------------------------------------
   Network
   --------------------------------------------------------- */

typedef struct {

    Station stations[MAX_STATIONS];

    int station_count;

    TrackSection track[MAX_STATIONS];

    int track_count;

    SignalBlock blocks[MAX_BLOCKS];

    int block_count;

} Railway;


/* ---------------------------------------------------------
   Simulator
   --------------------------------------------------------- */

typedef struct {

    Railway railway;

    Train trains[MAX_TRAINS];

    int train_count;

    Weather weather;

    double simulation_time;

} Simulator;


/* ---------------------------------------------------------
   Utility
   --------------------------------------------------------- */

static double clamp(
    double x,
    double lo,
    double hi)
{
    if (x < lo)
        return lo;

    if (x > hi)
        return hi;

    return x;
}


/* ---------------------------------------------------------
   Station creation
   --------------------------------------------------------- */

static void add_station(
    Railway *r,
    const char *name,
    double position,
    bool interchange)
{
    if (r->station_count >= MAX_STATIONS)
        return;

    Station *s =
        &r->stations[r->station_count++];

    snprintf(
        s->name,
        sizeof(s->name),
        "%s",
        name
    );

    s->position = position;

    s->platform_length = 100.0;

    s->passenger_demand = 30;

    s->passengers_waiting = 0;

    s->interchange = interchange;
}


/* ---------------------------------------------------------
   Track section
   --------------------------------------------------------- */

static void add_track(
    Railway *r,
    double start,
    double end,
    double speed,
    double gradient,
    int block)
{
    if (r->track_count >= MAX_STATIONS)
        return;

    TrackSection *t =
        &r->track[r->track_count++];

    t->start = start;
    t->end = end;
    t->speed_limit = speed;
    t->gradient = gradient;
    t->block = block;
}


/* ---------------------------------------------------------
   Find track section
   --------------------------------------------------------- */

static TrackSection *
get_track(
    Railway *r,
    double position)
{
    for (int i = 0;
         i < r->track_count;
         i++)
    {
        TrackSection *t =
            &r->track[i];

        if (position >= t->start &&
            position < t->end)
        {
            return t;
        }
    }

    return NULL;
}


/* ---------------------------------------------------------
   Network construction
   --------------------------------------------------------- */

static void build_network(
    Railway *r)
{
    memset(r, 0, sizeof(*r));

    /*
        Example 2030s city:

        North Terminal
            ↓
        University
        ↓
        Central
        ↓
        Financial District
        ↓
        Technology Park
        ↓
        Airport
    */

    add_station(
        r,
        "North Terminal",
        0.0,
        true
    );

    add_station(
        r,
        "University",
        1800.0,
        false
    );

    add_station(
        r,
        "Central",
        3600.0,
        true
    );

    add_station(
        r,
        "Financial District",
        5200.0,
        true
    );

    add_station(
        r,
        "Technology Park",
        7200.0,
        false
    );

    add_station(
        r,
        "Airport",
        9500.0,
        true
    );

    /*
        Track sections.
    */

    add_track(
        r,
        0,
        1800,
        27.78,
        0.005,
        0
    );

    add_track(
        r,
        1800,
        3600,
        33.33,
        -0.002,
        1
    );

    add_track(
        r,
        3600,
        5200,
        25.0,
        0.0,
        2
    );

    add_track(
        r,
        5200,
        7200,
        33.33,
        0.004,
        3
    );

    add_track(
        r,
        7200,
        9500,
        38.89,
        -0.003,
        4
    );

    r->block_count = 5;
}


/* ---------------------------------------------------------
   Train creation
   --------------------------------------------------------- */

static Train create_train(
    int id,
    const char *name,
    double position)
{
    Train t;

    memset(&t, 0, sizeof(t));

    t.id = id;

    snprintf(
        t.name,
        sizeof(t.name),
        "%s",
        name
    );

    t.position = position;

    t.velocity = 0;

    t.acceleration = 0;

    /*
        Six-car automated monorail.
    */

    t.length = 120.0;

    t.mass = 90000.0;

    t.capacity = 420;

    t.passengers = 100;

    t.passenger_mass =
        t.passengers * 75.0;

    /*
        4 MW traction system.
    */

    t.traction_power =
        4000000.0;

    /*
        5 MW regenerative braking.
    */

    t.braking_power =
        5000000.0;

    t.battery_capacity =
        2500.0;

    t.battery_kwh =
        2000.0;

    t.automatic = true;

    t.current_station = -1;

    return t;
}


/* ---------------------------------------------------------
   Passenger model
   --------------------------------------------------------- */

static void generate_passengers(
    Railway *r)
{
    for (int i = 0;
         i < r->station_count;
         i++)
    {
        Station *s =
            &r->stations[i];

        /*
            Simplified demand model.
        */

        int increase =
            s->passenger_demand / 100;

        s->passengers_waiting +=
            increase;

        if (s->passengers_waiting > 1000)
            s->passengers_waiting = 1000;
    }
}


/* ---------------------------------------------------------
   Passenger exchange
   --------------------------------------------------------- */

static void station_passenger_exchange(
    Train *t,
    Station *s)
{
    /*
        Passengers leaving.
    */

    int leaving =
        (int)(t->passengers * 0.12);

    t->passengers -= leaving;

    /*
        Passengers boarding.
    */

    int available =
        t->capacity -
        t->passengers;

    int boarding =
        s->passengers_waiting;

    if (boarding > available)
        boarding = available;

    t->passengers += boarding;

    s->passengers_waiting -=
        boarding;

    /*
        Passenger mass.
    */

    t->passenger_mass =
        t->passengers * 75.0;
}


/* ---------------------------------------------------------
   Find nearest station
   --------------------------------------------------------- */

static int find_station(
    Railway *r,
    double position)
{
    for (int i = 0;
         i < r->station_count;
         i++)
    {
        double d =
            fabs(
                position -
                r->stations[i].position
            );

        if (d < 15.0)
            return i;
    }

    return -1;
}


/* ---------------------------------------------------------
   Automatic train control
   --------------------------------------------------------- */

static double ato_target_speed(
    Simulator *sim,
    Train *t)
{
    TrackSection *section =
        get_track(
            &sim->railway,
            t->position
        );

    if (!section)
        return 0.0;

    double target =
        section->speed_limit;

    /*
        Station approach.
    */

    for (int i = 0;
         i < sim->railway.station_count;
         i++)
    {
        Station *s =
            &sim->railway.stations[i];

        double distance =
            s->position -
            t->position;

        /*
            Stop at the next station.
        */

        if (distance > 0 &&
            distance < 600.0)
        {
            /*
                Braking curve.
            */

            double braking =
                1.1;

            double safe_speed =
                sqrt(
                    2.0 *
                    braking *
                    distance
                );

            if (safe_speed < target)
                target = safe_speed;
        }
    }

    /*
        Weather restriction.
    */

    if (sim->weather.type ==
        WEATHER_STORM)
    {
        target *= 0.65;
    }

    if (sim->weather.type ==
        WEATHER_FOG)
    {
        target *= 0.80;
    }

    return target;
}


/* ---------------------------------------------------------
   Train physics
   --------------------------------------------------------- */

static void update_train(
    Simulator *sim,
    Train *t)
{
    TrackSection *section =
        get_track(
            &sim->railway,
            t->position
        );

    if (!section)
        return;

    /*
        Station dwell.
    */

    if (t->dwell_timer > 0)
    {
        t->dwell_timer -= DT;

        t->velocity = 0;

        t->acceleration = 0;

        return;
    }

    /*
        ATO target speed.
    */

    t->target_speed =
        ato_target_speed(
            sim,
            t
        );

    double error =
        t->target_speed -
        t->velocity;

    /*
        Automatic traction controller.
    */

    double acceleration =
        clamp(
            error * 0.7,
            -1.3,
            1.2
        );

    /*
        Gradient.
    */

    double gradient_force =
        GRAVITY *
        section->gradient;

    acceleration -=
        gradient_force;

    /*
        Weather resistance.
    */

    if (sim->weather.wind_speed > 15)
    {
        acceleration -=
            0.03;
    }

    /*
        Track resistance.
    */

    acceleration -=
        0.04;

    /*
        Energy.
    */

    if (acceleration > 0)
    {
        double power =
            t->mass *
            acceleration *
            t->velocity;

        double kw =
            power / 1000.0;

        double energy =
            kw * DT / 3600.0;

        t->battery_kwh -=
            energy;

        if (t->battery_kwh < 0)
            t->battery_kwh = 0;
    }

    /*
        Regenerative braking.
    */

    if (acceleration < 0 &&
        t->velocity > 5)
    {
        double recovered =
            fabs(acceleration) *
            t->mass *
            t->velocity *
            DT /
            3600000.0;

        recovered *= 0.75;

        t->regenerative_energy +=
            recovered;

        t->battery_kwh +=
            recovered;

        if (t->battery_kwh >
            t->battery_capacity)
        {
            t->battery_kwh =
                t->battery_capacity;
        }
    }

    /*
        Integrate physics.
    */

    t->acceleration =
        acceleration;

    t->velocity +=
        acceleration * DT;

    if (t->velocity < 0)
        t->velocity = 0;

    if (t->velocity >
        section->speed_limit)
    {
        t->velocity =
            section->speed_limit;
    }

    t->position +=
        t->velocity * DT;
}


/* ---------------------------------------------------------
   Station logic
   --------------------------------------------------------- */

static void update_station_logic(
    Simulator *sim,
    Train *t)
{
    int station =
        find_station(
            &sim->railway,
            t->position
        );

    if (station < 0)
        return;

    /*
        Only stop when travelling slowly.
    */

    if (t->velocity < 0.5)
    {
        if (t->current_station != station)
        {
            t->current_station =
                station;

            t->at_station = true;

            t->doors_open = true;

            t->dwell_timer =
                15.0;

            station_passenger_exchange(
                t,
                &sim->railway.stations[station]
            );
        }
    }
}


/* ---------------------------------------------------------
   Signalling
   --------------------------------------------------------- */

static void update_signalling(
    Simulator *sim)
{
    memset(
        sim->railway.blocks,
        0,
        sizeof(sim->railway.blocks)
    );

    for (int i = 0;
         i < sim->train_count;
         i++)
    {
        Train *t =
            &sim->trains[i];

        TrackSection *section =
            get_track(
                &sim->railway,
                t->position
            );

        if (!section)
            continue;

        int block =
            section->block;

        if (block >= 0 &&
            block < MAX_BLOCKS)
        {
            sim->railway.blocks[block]
                .occupied = true;

            sim->railway.blocks[block]
                .train_id = t->id;
        }
    }
}


/* ---------------------------------------------------------
   Weather
   --------------------------------------------------------- */

static void update_weather(
    Weather *w,
    double time)
{
    /*
        Slowly changing weather.
    */

    double cycle =
        sin(time / 600.0);

    w->wind_speed =
        8.0 +
        cycle * 6.0;

    w->temperature =
        14.0 +
        cycle * 4.0;

    if (cycle > 0.65)
    {
        w->type =
            WEATHER_RAIN;

        w->visibility =
            5000.0;
    }
    else
    {
        w->type =
            WEATHER_CLEAR;

        w->visibility =
            20000.0;
    }
}


/* ---------------------------------------------------------
   Simulation
   --------------------------------------------------------- */

static void simulator_update(
    Simulator *sim)
{
    update_weather(
        &sim->weather,
        sim->simulation_time
    );

    generate_passengers(
        &sim->railway
    );

    update_signalling(
        sim
    );

    for (int i = 0;
         i < sim->train_count;
         i++)
    {
        update_train(
            sim,
            &sim->trains[i]
        );

        update_station_logic(
            sim,
            &sim->trains[i]
        );
    }

    sim->simulation_time += DT;
}


/* ---------------------------------------------------------
   Initialisation
   --------------------------------------------------------- */

static void simulator_init(
    Simulator *sim)
{
    memset(sim, 0, sizeof(*sim));

    build_network(
        &sim->railway
    );

    sim->weather.type =
        WEATHER_CLEAR;

    sim->weather.temperature =
        15.0;

    sim->weather.visibility =
        20000.0;

    /*
        Fleet.
    */

    sim->train_count = 4;

    sim->trains[0] =
        create_train(
            1,
            "M-01",
            0.0
        );

    sim->trains[1] =
        create_train(
            2,
            "M-02",
            -600.0
        );

    sim->trains[2] =
        create_train(
            3,
            "M-03",
            -1200.0
        );

    sim->trains[3] =
        create_train(
            4,
            "M-04",
            -1800.0
        );
}


/* ---------------------------------------------------------
   Main
   --------------------------------------------------------- */

int main(void)
{
    Simulator sim;

    simulator_init(
        &sim
    );

    printf(
        "\n"
        "=====================================\n"
        "   2030s URBAN MONORAIL SIMULATOR\n"
        "=====================================\n\n"
    );

    /*
        Run 30 minutes.
    */

    const int frames =
        30 * 60 * 10;

    for (int frame = 0;
         frame < frames;
         frame++)
    {
        simulator_update(
            &sim
        );

        /*
            Print every 10 seconds.
        */

        if (frame % 100 == 0)
        {
            Train *t =
                &sim.trains[0];

            printf(
                "TIME %6.1fs | "
                "TRAIN %s | "
                "POS %7.1fm | "
                "SPEED %6.1f km/h | "
                "PASS %3d | "
                "BATTERY %6.1f kWh | "
                "REGEN %6.1f kWh\n",

                sim.simulation_time,

                t->name,

                t->position,

                t->velocity * 3.6,

                t->passengers,

                t->battery_kwh,

                t->regenerative_energy
            );
        }
    }

    printf(
        "\nSimulation complete.\n"
    );

    return 0;
}





