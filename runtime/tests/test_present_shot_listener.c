#include "present_shot_listener.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;
static int request_result = 1;
static int sequence = 41;
static int wrote = 1;
static char requested_path[512];

int present_shot_request(const char *path)
{
    snprintf(requested_path, sizeof(requested_path), "%s", path ? path : "");
    return request_result;
}
int present_shot_seq(void) { return sequence; }
int present_shot_ok(void) { return wrote; }

#define CHECK(expr) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); } } while (0)

static void process(const char *request, char response[2048])
{
    int n = present_shot_listener_process_request(request, response, 2048);
    CHECK(n > 0);
    CHECK(response[n - 1] == '\n');
    CHECK(response[n] == '\0');
}

int main(void)
{
    char response[2048];

    process("{\"id\":7,\"cmd\":\"present_shot_seq\"}", response);
    CHECK(strcmp(response,
        "{\"id\":7,\"ok\":true,\"seq\":41,\"wrote\":1}\n") == 0);

    process("{\"id\":8,\"cmd\":\"present_shot\",\"path\":\"C:\\\\proof\\\\frame.png\"}",
            response);
    CHECK(strcmp(requested_path, "C:\\proof\\frame.png") == 0);
    CHECK(strstr(response, "\"staged\":true") != NULL);
    CHECK(strstr(response, "\"path\":\"C:\\\\proof\\\\frame.png\"") != NULL);
    CHECK(strstr(response, "\"seq\":41") != NULL);

    request_result = 0;
    process("{\"id\":9,\"cmd\":\"present_shot\"}", response);
    CHECK(strcmp(requested_path, "psx_present_shot.png") == 0);
    CHECK(strstr(response, "\"ok\":false") != NULL);
    CHECK(strstr(response, "present_shot unavailable") != NULL);

    process("{\"id\":10,\"cmd\":\"read_ram\"}", response);
    CHECK(strstr(response, "\"ok\":false") != NULL);
    CHECK(strstr(response, "unknown command") != NULL);

    process("{\"id\":11}", response);
    CHECK(strstr(response, "missing command") != NULL);

    if (failures) {
        fprintf(stderr, "present_shot_listener_test: %d/%d failed\n", failures, checks);
        return 1;
    }
    printf("present_shot_listener_test: PASS (%d checks)\n", checks);
    return 0;
}
