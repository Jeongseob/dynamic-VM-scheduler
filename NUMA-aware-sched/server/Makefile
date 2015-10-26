TARGET = server 
OBJS = core.o crew.o sock.o collect.o
LIBS = -lnsl -lpthread -lrt 
#OPT = -xinstrument=datarace
DEFINES = -DCREW_SIZE=4

CC = gcc

SRCS = $(OBJS:.o=.c) 
CFLAGS = -Wall -O2 -g

# 
# Compilation target for C files 
# 
%.o:%.c 
	@echo "Compiling $< ..." 
	$(CC) -c $(CFLAGS) -o $@ $< $(DEFINES) #$(OPT)

all : $(TARGET) 
$(TARGET) : $(OBJS) 
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LIBS) 
clean : 
	rm -rf $(OBJS) $(TARGET) core 
