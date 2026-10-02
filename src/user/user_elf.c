static inline void sys_print(const char *msg, int iters, char spin) {
    asm volatile (
        "int $0x80"
        :
        : "a"(1), "b"(msg), "c"(iters), "d"((unsigned int)spin)
        : "memory"
    );
}

static inline void sys_exit(int code) {
    asm volatile ("int $0x80" : : "a"(2), "b"(code) : "memory");
    while (1);
}

static inline int sys_fork(void) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(3) : "memory");
    return ret;
}

static inline int sys_pipe_write(const char *buf, int len) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(4), "b"(buf), "c"(len) : "memory");
    return ret;
}

static inline int sys_pipe_read(char *buf, int max_len) {
    int ret;
    asm volatile ("int $0x80" : "=a"(ret) : "a"(5), "b"(buf), "c"(max_len) : "memory");
    return ret;
}

void main(void) {
    int pid = sys_fork();

    if (pid == 0) {
        // Потомок (PID 3)
        const char *ipc_msg = "Pipe_Message_Delivered!";
        sys_pipe_write(ipc_msg, 23);

        for (int i = 1; i <= 60; i++) {
            sys_print("Child sent IPC to pipe", i, '/');
            for (volatile int d = 0; d < 20000; d++);
        }
        sys_exit(10);
    } else {
        // Родитель (PID 2)
        // volatile предотвращает оптимизацию цикла в векторную инструкцию movaps
        char recv_buf[32];
        volatile char *vptr = (volatile char *)recv_buf;
        for (int i = 0; i < 32; i++) vptr[i] = 0;

        for (volatile int d = 0; d < 80000; d++);

        int bytes = sys_pipe_read(recv_buf, 31);
        if (bytes > 0) {
            recv_buf[bytes] = '\0';
        } else {
            recv_buf[0] = 'E'; recv_buf[1] = 'R'; recv_buf[2] = 'R'; recv_buf[3] = '\0';
        }

        for (int i = 1; i <= 120; i++) {
            sys_print(recv_buf, i, '|');
            for (volatile int d = 0; d < 20000; d++);
        }
        sys_exit(20);
    }
}
