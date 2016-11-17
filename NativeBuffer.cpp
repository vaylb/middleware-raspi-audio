#define LOG_TAG "NativeBuffer"
#define DGB 1
#include"NativeBuffer.h"

NativeBuffer::NativeBuffer(size_t size){
	printf("create NativeBuffer\n");
	buffer=(char*)malloc(size);
	bufsize=size;
	write_ptr=0;
	read_ptr=0;
}
NativeBuffer::~NativeBuffer(){
	if(buffer!=NULL)
		free(buffer);
	printf("delete NativeBuffer");
}
size_t NativeBuffer::getReadSpace(){
	return write_ptr-read_ptr;
}

size_t NativeBuffer::getWriteSpace(){
	return bufsize-(write_ptr-read_ptr);
}

void NativeBuffer::setWritePos(size_t pos){
	write_ptr=pos;
}

size_t NativeBuffer::Read( char *dest, size_t cnt){
	size_t curptr=read_ptr%bufsize;
	if(bufsize-curptr>=cnt){
		memcpy(dest,buffer+curptr,cnt);
		read_ptr+=cnt;
		return cnt;
	}else{
		size_t n1=bufsize-curptr;
		memcpy(dest,buffer+curptr,n1);
		size_t n2=cnt-n1;
		memcpy(dest+n1,buffer,n2);
		read_ptr+=cnt;
		return cnt;
	}
	
}

size_t NativeBuffer::Write(char *src, size_t cnt){
	size_t capacity = getWriteSpace();
	if(capacity>=cnt){
		size_t curptr=write_ptr%bufsize;
		if(bufsize-curptr>=cnt){
			memcpy(buffer+curptr,src,cnt);
			write_ptr+=cnt;
			return cnt;
		}else{
			size_t n1=bufsize-curptr;
			memcpy(buffer+curptr,src,n1);
			size_t n2=cnt-n1;
			memcpy(buffer,src+n1,n2);
			write_ptr+=cnt;
			return cnt;
		}
	}else return 0;
	
}

void NativeBuffer::Reset(){
	read_ptr=0;
	write_ptr=0;
	memset(buffer,0,bufsize);
}
