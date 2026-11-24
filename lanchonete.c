#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ITEM_COUNT 5

static const char *const item_names[ITEM_COUNT] = {
    "hamburguer", "pizza", "salada", "suco", "batata frita"
};

typedef struct {
    int id;
    int item;
    int is_stop;
} Pedido;

typedef struct {
    Pedido *items;
    size_t capacity;
    size_t head;
    size_t tail;
    sem_t spaces;
    sem_t available;
    pthread_mutex_t mutex;
    pthread_mutex_t output_mutex;
} Fila;

typedef struct {
    Fila *fila;
    int id;
} CozinheiroArgs;

static int parse_positive(const char *text, long min, long max, long *value) {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return 0;
    }
    *value = parsed;
    return 1;
}

static int write_full(int fd, const void *buffer, size_t size) {
    const char *cursor = buffer;
    size_t written = 0;
    while (written < size) {
        ssize_t result = write(fd, cursor + written, size - written);
        if (result < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (result == 0) return 0;
        written += (size_t)result;
    }
    return 1;
}

static int read_pedido(int fd, Pedido *pedido) {
    char *cursor = (char *)pedido;
    size_t received = 0;
    while (received < sizeof(*pedido)) {
        ssize_t result = read(fd, cursor + received, sizeof(*pedido) - received);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (result == 0) return received == 0 ? 0 : -1;
        received += (size_t)result;
    }
    return 1;
}

static void pause_ms(long milliseconds) {
    struct timespec remaining = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L
    };
    while (nanosleep(&remaining, &remaining) == -1 && errno == EINTR) {
    }
}

static int semaphore_wait(sem_t *semaphore) {
    while (sem_wait(semaphore) == -1) {
        if (errno != EINTR) return 0;
    }
    return 1;
}

static int fila_inserir(Fila *fila, Pedido pedido) {
    if (!semaphore_wait(&fila->spaces)) return 0;
    pthread_mutex_lock(&fila->mutex);
    fila->items[fila->tail] = pedido;
    fila->tail = (fila->tail + 1) % fila->capacity;
    pthread_mutex_unlock(&fila->mutex);
    return sem_post(&fila->available) == 0;
}

static int fila_remover(Fila *fila, Pedido *pedido) {
    if (!semaphore_wait(&fila->available)) return 0;
    pthread_mutex_lock(&fila->mutex);
    *pedido = fila->items[fila->head];
    fila->head = (fila->head + 1) % fila->capacity;
    pthread_mutex_unlock(&fila->mutex);
    return sem_post(&fila->spaces) == 0;
}

static void *cozinheiro(void *argument) {
    CozinheiroArgs *args = argument;
    Fila *fila = args->fila;
    for (;;) {
        Pedido pedido;
        if (!fila_remover(fila, &pedido)) {
            pthread_mutex_lock(&fila->output_mutex);
            fprintf(stderr, "Erro ao retirar pedido da fila.\n");
            pthread_mutex_unlock(&fila->output_mutex);
            return NULL;
        }
        if (pedido.is_stop) break;

        pthread_mutex_lock(&fila->output_mutex);
        printf("Cozinheiro %d preparando pedido %d: %s.\n",
               args->id, pedido.id, item_names[pedido.item]);
        fflush(stdout);
        pthread_mutex_unlock(&fila->output_mutex);

        pause_ms(250 + (pedido.id * 137L + args->id * 83L) % 551L);

        pthread_mutex_lock(&fila->output_mutex);
        printf("Cozinheiro %d entregou pedido %d: %s.\n",
               args->id, pedido.id, item_names[pedido.item]);
        fflush(stdout);
        pthread_mutex_unlock(&fila->output_mutex);
    }
    return NULL;
}

static int validar_pedido(Pedido pedido, int total) {
    return !pedido.is_stop && pedido.id >= 1 && pedido.id <= total &&
           pedido.item >= 0 && pedido.item < ITEM_COUNT;
}

int main(int argc, char **argv) {
    long total_long = 20;
    long cozinheiros_long = 3;
    long capacidade_long = 4;
    if (argc > 4 ||
        (argc > 1 && !parse_positive(argv[1], 1, 10000, &total_long)) ||
        (argc > 2 && !parse_positive(argv[2], 1, 64, &cozinheiros_long)) ||
        (argc > 3 && !parse_positive(argv[3], 1, 10000, &capacidade_long))) {
        fprintf(stderr, "Uso: %s [quantidade_pedidos [cozinheiros [capacidade_fila]]]\n", argv[0]);
        return EXIT_FAILURE;
    }

    int total = (int)total_long;
    int cozinheiros_count = (int)cozinheiros_long;
    Fila fila = {.capacity = (size_t)capacidade_long};
    fila.items = calloc(fila.capacity, sizeof(*fila.items));
    pthread_t *threads = calloc((size_t)cozinheiros_count, sizeof(*threads));
    CozinheiroArgs *args = calloc((size_t)cozinheiros_count, sizeof(*args));
    if (!fila.items || !threads || !args) {
        perror("calloc");
        free(fila.items);
        free(threads);
        free(args);
        return EXIT_FAILURE;
    }

    if (sem_init(&fila.spaces, 0, (unsigned int)fila.capacity) == -1 ||
        sem_init(&fila.available, 0, 0) == -1 ||
        pthread_mutex_init(&fila.mutex, NULL) != 0 ||
        pthread_mutex_init(&fila.output_mutex, NULL) != 0) {
        perror("inicializacao da sincronizacao");
        free(fila.items);
        free(threads);
        free(args);
        return EXIT_FAILURE;
    }

    int pipe_fds[2];
    if (pipe(pipe_fds) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    pid_t produtor = fork();
    if (produtor == -1) {
        perror("fork");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return EXIT_FAILURE;
    }
    if (produtor == 0) {
        close(pipe_fds[0]);
        srand((unsigned int)(time(NULL) ^ getpid()));
        for (int id = 1; id <= total; id++) {
            Pedido pedido = {.id = id, .item = rand() % ITEM_COUNT, .is_stop = 0};
            if (!write_full(pipe_fds[1], &pedido, sizeof(pedido))) {
                close(pipe_fds[1]);
                _exit(EXIT_FAILURE);
            }
            pause_ms(80 + rand() % 171);
        }
        close(pipe_fds[1]);
        _exit(EXIT_SUCCESS);
    }

    close(pipe_fds[1]);
    int criadas = 0;
    for (int i = 0; i < cozinheiros_count; i++) {
        args[i].fila = &fila;
        args[i].id = i + 1;
        int error = pthread_create(&threads[i], NULL, cozinheiro, &args[i]);
        if (error != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            break;
        }
        criadas++;
    }

    pthread_mutex_lock(&fila.output_mutex);
    printf("Lanchonete aberta: %d pedidos, %d cozinheiro(s), fila para %zu pedido(s).\n",
           total, criadas, fila.capacity);
    pthread_mutex_unlock(&fila.output_mutex);

    int sucesso = criadas == cozinheiros_count;
    if (!sucesso) close(pipe_fds[0]);
    int recebidos = 0;
    if (sucesso) {
        Pedido pedido;
        int status;
        while ((status = read_pedido(pipe_fds[0], &pedido)) == 1) {
            if (!validar_pedido(pedido, total) || !fila_inserir(&fila, pedido)) {
                fprintf(stderr, "Pedido invalido ou falha ao inserir na fila.\n");
                sucesso = 0;
                break;
            }
            recebidos++;
            pthread_mutex_lock(&fila.output_mutex);
            printf("Pedido %d recebido: %s.\n", pedido.id, item_names[pedido.item]);
            pthread_mutex_unlock(&fila.output_mutex);
        }
        if (status == -1) {
            fprintf(stderr, "Falha ao ler pedido enviado pelo processo cliente.\n");
            sucesso = 0;
        }
        close(pipe_fds[0]);
    }

    for (int i = 0; i < criadas; i++) {
        Pedido stop = {.id = 0, .item = 0, .is_stop = 1};
        if (!fila_inserir(&fila, stop)) sucesso = 0;
    }
    for (int i = 0; i < criadas; i++) pthread_join(threads[i], NULL);

    int child_status = 0;
    while (waitpid(produtor, &child_status, 0) == -1 && errno == EINTR) {
    }
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != EXIT_SUCCESS) sucesso = 0;

    pthread_mutex_lock(&fila.output_mutex);
    if (sucesso && recebidos == total) {
        printf("Simulacao concluida: %d pedido(s) recebidos e preparados.\n", recebidos);
    } else {
        printf("Simulacao encerrada com falha: %d de %d pedido(s) recebidos.\n", recebidos, total);
    }
    pthread_mutex_unlock(&fila.output_mutex);

    pthread_mutex_destroy(&fila.output_mutex);
    pthread_mutex_destroy(&fila.mutex);
    sem_destroy(&fila.available);
    sem_destroy(&fila.spaces);
    free(fila.items);
    free(threads);
    free(args);
    return sucesso && recebidos == total ? EXIT_SUCCESS : EXIT_FAILURE;
}
