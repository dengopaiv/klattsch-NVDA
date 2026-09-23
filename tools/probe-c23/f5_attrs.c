[[nodiscard]] static int f(void){ return 0; }
int main(void){ [[maybe_unused]] int u = 0; return f(); }
