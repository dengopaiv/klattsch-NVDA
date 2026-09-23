static const unsigned char blob[] = {
#embed "data.bin"
};
int main(void){ return (sizeof blob == 3 && blob[0]=='A') ? 0 : 1; }
