casames


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 8080
#define BUFFER_SIZE 8192

void send_response(int client, const char *status,
                   const char *content_type,
                   const char *body)
{
    char response[BUFFER_SIZE];

    snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status,
        content_type,
        strlen(body),
        body
    );

    send(client, response, strlen(response), 0);
}

void handle_request(int client)
{
    char buffer[BUFFER_SIZE];

    memset(buffer, 0, sizeof(buffer));

    recv(client, buffer, sizeof(buffer) - 1, 0);

    printf("Solicitud recibida:\n%s\n", buffer);

    if (strncmp(buffer, "GET / ", 6) == 0)
    {
        FILE *file = fopen("public/index.html", "r");

        if (!file)
        {
            send_response(
                client,
                "500 Internal Server Error",
                "text/plain; charset=utf-8",
                "Error interno del servidor"
            );

            return;
        }

        char *html = malloc(50000);

        if (!html)
        {
            fclose(file);
            return;
        }

        size_t size = fread(html, 1, 49999, file);
        html[size] = '\0';

        fclose(file);

        send_response(
            client,
            "200 OK",
            "text/html; charset=utf-8",
            html
        );

        free(html);
    }

    else if (strncmp(buffer, "GET /api/casas", 14) == 0)
    {
        const char *houses =
            "["
            "{\"id\":1,\"ciudad\":\"Málaga\",\"precio\":1850,\"habitaciones\":2},"
            "{\"id\":2,\"ciudad\":\"Valencia\",\"precio\":1600,\"habitaciones\":3},"
            "{\"id\":3,\"ciudad\":\"Mallorca\",\"precio\":2400,\"habitaciones\":2}"
            "]";

        send_response(
            client,
            "200 OK",
            "application/json; charset=utf-8",
            houses
        );
    }

    else
    {
        send_response(
            client,
            "404 Not Found",
            "text/plain; charset=utf-8",
            "Página no encontrada"
        );
    }
}

int main(void)
{
    int server_fd;
    int client_fd;

    struct sockaddr_in address;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0)
    {
        perror("No se pudo crear el socket");
        return 1;
    }

    int reuse = 1;

    setsockopt(
        server_fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse)
    );

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(
        server_fd,
        (struct sockaddr *)&address,
        sizeof(address)
    ) < 0)
    {
        perror("No se pudo iniciar el servidor");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0)
    {
        perror("No se pudo escuchar");
        close(server_fd);
        return 1;
    }

    printf("CasaMes iniciado.\n");
    printf("Servidor: http://localhost:%d\n", PORT);

    while (1)
    {
        client_fd = accept(server_fd, NULL, NULL);

        if (client_fd < 0)
            continue;

        handle_request(client_fd);

        close(client_fd);
    }

    close(server_fd);

    return 0;
}





#ifndef PROPERTIES_H
#define PROPERTIES_H

#define MAX_PROPERTIES 100

typedef struct
{
    int id;

    char city[64];
    char title[128];

    int bedrooms;
    int bathrooms;

    double monthly_price;

    int available;
} Property;

void load_properties(void);

Property *get_property(int id);

int get_property_count(void);

#endif




#include "properties.h"
#include <string.h>

static Property properties[MAX_PROPERTIES];

static int property_count = 0;

void load_properties(void)
{
    properties[0] = (Property)
    {
        1,
        "Málaga",
        "Casa moderna cerca del mar",
        2,
        2,
        1850.00,
        1
    };

    properties[1] = (Property)
    {
        2,
        "Valencia",
        "Casa luminosa en Valencia",
        3,
        2,
        1600.00,
        1
    };

    properties[2] = (Property)
    {
        3,
        "Mallorca",
        "Villa tranquila en Mallorca",
        2,
        2,
        2400.00,
        1
    };

    property_count = 3;
}

Property *get_property(int id)
{
    for (int i = 0; i < property_count; i++)
    {
        if (properties[i].id == id)
            return &properties[i];
    }

    return NULL;
}

int get_property_count(void)
{
    return property_count;
}


#ifndef BOOKINGS_H
#define BOOKINGS_H

typedef struct
{
    int booking_id;
    int property_id;

    char guest_name[100];
    char email[150];

    char start_date[11];
    char end_date[11];

    int guests;

    double total_price;
} Booking;

int create_booking(
    int property_id,
    const char *guest_name,
    const char *email,
    const char *start_date,
    const char *end_date,
    int guests,
    double price
);

#endif

#include "bookings.h"

#include <stdio.h>
#include <string.h>

static int next_booking_id = 1;

int create_booking(
    int property_id,
    const char *guest_name,
    const char *email,
    const char *start_date,
    const char *end_date,
    int guests,
    double price
)
{
    FILE *file = fopen(
        "data/bookings.txt",
        "a"
    );

    if (!file)
        return -1;

    int booking_id = next_booking_id++;

    fprintf(
        file,
        "%d|%d|%s|%s|%s|%s|%d|%.2f\n",
        booking_id,
        property_id,
        guest_name,
        email,
        start_date,
        end_date,
        guests,
        price
    );

    fclose(file);

    return booking_id;
}


