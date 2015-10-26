TARGET = scheduler 
OBJS = scheduler.o crew.o virtualMachine.o
LIBS = -lpthread -lrt
#OPT = -xinstrument=datarace
DEFINES = -DCREW_SIZE=10

CC = g++

SRCS = $(OBJS:.o=.c) 
CFLAGS = -Wall -O2 -g

# 
# Compilation target for C files 
# 
%.o:%.cpp 
	@echo "Compiling $< ..." 
	$(CC) -c $(CFLAGS) -o $@ $< $(DEFINES) $(OPT)

all : $(TARGET) 
$(TARGET) : $(OBJS) 
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LIBS) 
clean : 
	rm -rf $(OBJS) $(TARGET) core 
