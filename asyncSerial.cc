#include <node.h>	/* �K�{ */
#include <uv.h>		/* �񓯊��R�[���o�b�N�̈� */
using namespace v8;	/* �K�{ */

#define SIZE_RCV_BUFFER	(1024)

/* �X���b�h�ԏ��(���M) */
struct send_inf
{
	int timeout;					/* �^�C���A�E�g�l */
	Persistent<Function> callback;	/* �񓯊����������R�[���o�b�N�֐� */
	int result;						/* ����(�ҋ@�����b) */
	int length;						/* ���M������ */
	String::Utf8Value *buf;			/* ���M������ */
};
/* �X���b�h�ԏ��(��M) */
struct recv_inf
{
	int timeout;					/* �^�C���A�E�g�l */
	Persistent<Function> callback;	/* �񓯊����������R�[���o�b�N�֐� */
	int result;						/* ����(�ҋ@�����b) */
	int length;						/* ��M������ */
	char buf[SIZE_RCV_BUFFER + 1];		/* ��M���� */
	DWORD signal;
};

HANDLE hRs232c;
CRITICAL_SECTION secSend;
CRITICAL_SECTION secRecv;
bool bInitialized = false;

char * ToCString(v8::String::Utf8Value &value)
{
	return *value ? *value : "<string conversion failed>";
}
#define RESULT_OK	(0)
#define RESULT_ERR_SND_CREATE_EVENT		(-1)
#define RESULT_ERR_SND_WRITE_FILE		(-2)
#define RESULT_ERR_SND_OVERLAP			(-3)
#define RESULT_ERR_SND_WAIT_OBJECT		(-4)

#define RESULT_TIMEOUT					( 1)
#define RESULT_ERR_RCV_CREATE_EVENT		(-1)
#define RESULT_ERR_RCV_SET_COMM_MASK	(-2)
#define RESULT_ERR_RCV_WAIT_COMM_EVENT	(-3)
#define RESULT_ERR_RCV_READ_FILE		(-4)

/* �o�b�N�O���E���h�������s�֐� */
void _sendWait(uv_work_t* req) {

	EnterCriticalSection(&secSend);	/* ��d���M�����h�~ */
	send_inf* inf = static_cast<send_inf*>(req->data);	/* �������p�� */
	char *snd = ToCString(*inf->buf);

	OVERLAPPED ovWrite;
	DWORD dwWritten;
	memset(&ovWrite, 0, sizeof(ovWrite));
	ovWrite.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (ovWrite.hEvent == NULL){
		inf->result = RESULT_ERR_SND_CREATE_EVENT;
		goto LABEL_END;
	}
	if (!WriteFile(hRs232c, snd, strlen(snd), &dwWritten, &ovWrite)){
		if (GetLastError() != ERROR_IO_PENDING){
			inf->result = RESULT_ERR_SND_WRITE_FILE;
			goto LABEL_END;
		}
		DWORD res = WaitForSingleObject(ovWrite.hEvent, inf->timeout);
		switch (res){
		case WAIT_OBJECT_0:
			if (!GetOverlappedResult(hRs232c, &ovWrite, &dwWritten, FALSE))	{
				inf->result = RESULT_ERR_SND_OVERLAP;
				goto LABEL_END;
			}
			else{
				inf->result = RESULT_OK;
				goto LABEL_END;
			}
			break;
		default:
			inf->result = RESULT_ERR_SND_WAIT_OBJECT;
			goto LABEL_END;
		}
	}
	else{
		inf->result = RESULT_OK;
		goto LABEL_END;
	}
LABEL_END:
	CloseHandle(ovWrite.hEvent);
	LeaveCriticalSection(&secSend);
	return;
}
/* �o�b�N�O���E���h���������֐� */
void _sendEnd(uv_work_t *req,int status) {
	HandleScope scope;

	send_inf* inf = static_cast<send_inf*>(req->data);	/* �������p�� */
	Local<Value> argv[1] = { Number::New(inf->result) };		/* �R�[���o�b�N�p�����ݒ� */
	inf->callback->Call(Context::GetCurrent()->Global(), 1, argv);
	/* �㏈�� */
	delete inf;
	delete req;
}
/* �o�b�N�O���E���h��M�������s�֐� */
void _recvWait(uv_work_t* req) {

	EnterCriticalSection(&secRecv);	/* ��d��M�����h�~ */
	recv_inf* inf = static_cast<recv_inf*>(req->data);	/* �������p�� */

	OVERLAPPED ovRead;
	memset(&ovRead, 0, sizeof(ovRead));
	ovRead.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (ovRead.hEvent == NULL){
		inf->result = RESULT_ERR_RCV_CREATE_EVENT;
		goto LABEL_END;
	}
	if (SetCommMask(hRs232c, EV_RXCHAR | EV_CTS | EV_DSR) == FALSE){
		inf->result = RESULT_ERR_RCV_SET_COMM_MASK;
		goto LABEL_END;
	}
	DWORD bgn = GetTickCount();
	DWORD end;
LABEL_WAIT_RETRY_ZERO:
	DWORD dwEvtMask = 0;
	DWORD dwTransfer = 0;
	if (!WaitCommEvent(hRs232c, &dwEvtMask, &ovRead)){
		if (GetLastError() == ERROR_IO_PENDING) {
		LABEL_WAIT:
			bool flg = (inf->timeout == 0);
			if (!GetOverlappedResult(hRs232c, &ovRead, &dwTransfer, flg)){
				end = GetTickCount();
				if (inf->timeout > (int)(end - bgn)){
					Sleep(1);
					goto LABEL_WAIT;
				}
				inf->result = RESULT_TIMEOUT;	/* �^�C���A�E�g */
				goto LABEL_END;
			}
		}
		else{
			inf->result = RESULT_ERR_RCV_WAIT_COMM_EVENT;
			goto LABEL_END;
		}
	}
	inf->length = 0;
	GetCommModemStatus(hRs232c, &inf->signal);
	if ((dwEvtMask & EV_RXCHAR) == EV_RXCHAR){
		COMSTAT comst;
		DWORD dwErr;
		ClearCommError(hRs232c, &dwErr, &comst);
		if (comst.cbInQue == 0) goto LABEL_WAIT_RETRY_ZERO;
		if (comst.cbInQue > SIZE_RCV_BUFFER)comst.cbInQue = SIZE_RCV_BUFFER;
		if (!ReadFile(hRs232c, inf->buf, comst.cbInQue, (LPDWORD)&(inf->length), &ovRead)){
			inf->result = RESULT_ERR_RCV_READ_FILE;
			goto LABEL_END;
		}
	}
	inf->buf[inf->length] = 0;
	inf->result = RESULT_OK;
	goto LABEL_END;
LABEL_END:
	CloseHandle(ovRead.hEvent);
	LeaveCriticalSection(&secRecv);
	return;
}
/* �o�b�N�O���E���h���M���������֐� */
void _recvEnd(uv_work_t *req, int status) {
	HandleScope scope;

	recv_inf* inf = static_cast<recv_inf*>(req->data);	/* �������p�� */
	Local<Value> argv[3] = { 
		Number::New(inf->result),
		Number::New(inf->signal),
		String::New(inf->buf)
	};		/* �R�[���o�b�N�p�����ݒ� */
	int argc = (inf->result == 0) ? 3 : 1;
	inf->callback->Call(Context::GetCurrent()->Global(), argc, argv);
	/* �㏈�� */
	delete inf;
	delete req;
}
//------------------------------------------------------------------------------
//[�֐�]�V���A���|�[�g�̃I�[�v��
//[����]args[0]	COM�|�[�g��(string)
//		args[1] �{�[���[�g(Number)
//		args[2] �f�[�^�r�b�g��(Number)
//		args[3] �X�g�b�v�r�b�g��(Number)
//		args[4] �p���e�B(Number)
//[�ߒl]0:	����
//------------------------------------------------------------------------------
Handle<Value> serialOpen(const Arguments& args) {
	HandleScope scope;

	HANDLE hdl;

	/* �����̐��`�F�b�N */
	if(args.Length() != 5){
		ThrowException(Exception::TypeError(String::New("Wrong number of arguments")));
		return scope.Close(Undefined());
	}
	/* �����̌^�`�F�b�N */
	if(!args[0]->IsString() ||
	   !args[1]->IsNumber() ||
	   !args[2]->IsNumber() ||
	   !args[3]->IsNumber() ||
	   !args[4]->IsNumber()){
		ThrowException(Exception::TypeError(String::New("Wrong arguments")));
		return scope.Close(Undefined());
	}
	/* �������o */
	String::Utf8Value strCom(args[0]);
	char *pcCom = *strCom;
	int iBoudRate = (int)args[1]->NumberValue();
	int iByteSize = (int)args[2]->NumberValue();
	int iStopBits = (int)args[3]->NumberValue();
	int iParity = (int)args[4]->NumberValue();

	/* �n���h���擾 */
	hdl = CreateFile(
        pcCom,GENERIC_READ|GENERIC_WRITE,0,NULL,
        OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED,NULL);
	if (hdl == INVALID_HANDLE_VALUE) {
		ThrowException(Exception::TypeError(String::New("CreateFile(INVALID_HANDLE_VALUE)")));
		return scope.Close(Undefined());
    }
	/* COM�|�[�g�̃Z�b�g�A�b�v */
	if (!SetupComm(hdl, 1024, 1024)){
		ThrowException(Exception::TypeError(String::New("SetupComm(ERROR)")));
		return scope.Close(Undefined());
	}
	/* �|�[�g�̐ݒ�ύX */
	DCB dcb;
	memset(&dcb, NULL, sizeof(DCB));
	dcb.DCBlength = sizeof(DCB);
	GetCommState(hdl, &dcb);
	dcb.BaudRate = iBoudRate;
	dcb.Parity = iParity;
	dcb.StopBits = iStopBits;
	dcb.ByteSize = iByteSize;
	if (!SetCommState(hdl, &dcb)){
		ThrowException(Exception::TypeError(String::New("SetCommState(ERROR)")));
		return scope.Close(Undefined());
	}
	InitializeCriticalSection(&secSend);
	InitializeCriticalSection(&secRecv);
	bInitialized = true;
	hRs232c = hdl;
	/* ����M��OFF */
	EscapeCommFunction(hRs232c, CLRDTR);
	EscapeCommFunction(hRs232c, CLRRTS);
	return scope.Close(Number::New(0));
}
//------------------------------------------------------------------------------
//[�֐�]�V���A���|�[�g�̃N���[�Y
//[����]�Ȃ�
//[�ߒl]0:	����
//------------------------------------------------------------------------------
Handle<Value> serialClose(const Arguments& args) {
	HandleScope scope;

	CloseHandle(hRs232c);
	DeleteCriticalSection(&secSend);
	DeleteCriticalSection(&secRecv);
	bInitialized = false;
	return scope.Close(Number::New(0));
}
//------------------------------------------------------------------------------
//[�֐�]�V���A���|�[�g�̑��M
//[����]args[0]	���M������(string)
//		args[1] �^�C���A�E�g(ms)(Number)
//		args[2] �����R�[���o�b�N�֐�(Function)
//[�ߒl]0:	����
//------------------------------------------------------------------------------
Handle<Value> serialSend(const Arguments& args) {
	HandleScope scope;

	/* COM�|�[�g�`�F�b�N */
	if (!bInitialized){
		ThrowException(Exception::TypeError(String::New("Not open")));
		return scope.Close(Undefined());
	}
	/* �����`�F�b�N(�����̐�) */
	if (args.Length() != 3){
		ThrowException(Exception::TypeError(String::New("Wrong number of arguments")));
		return scope.Close(Undefined());
	}
	/* �����`�F�b�N(�����̌^) */
	if (!args[0]->IsString() || !args[1]->IsNumber() || !args[2]->IsFunction()) {
		ThrowException(Exception::TypeError(String::New("Wrong arguments")));
		return scope.Close(Undefined());
	}
	/* �����̏����L�� */
	send_inf *inf = new send_inf;
	inf->buf = new String::Utf8Value(args[0]);
	inf->timeout = args[1]->Int32Value();
	inf->callback = Persistent<Function>::New(Local<Function>::Cast(args[2]));
	inf->result = 0;

	/* �o�b�N�O���E���h�Ŏ��s */
	uv_work_t *req_send = new uv_work_t;
	req_send->data = inf;
	uv_queue_work(uv_default_loop(), req_send, _sendWait, _sendEnd);
	/* �Ƃ肠���������͖߂� */
	return scope.Close(Number::New(inf->buf->length()));
}
//------------------------------------------------------------------------------
//[�֐�]�V���A���|�[�g�̎�M
//[����]args[0] �^�C���A�E�g(ms)(Number)
//		args[1] �����R�[���o�b�N�֐�(Function)
//[�ߒl]0:	����
//------------------------------------------------------------------------------
Handle<Value> serialRecv(const Arguments& args) {
	HandleScope scope;

	/* COM�|�[�g�`�F�b�N */
	if (!bInitialized){
		ThrowException(Exception::TypeError(String::New("Not open")));
		return scope.Close(Undefined());
	}
	/* �����`�F�b�N(�����̐�) */
	if (args.Length() != 2){
		ThrowException(Exception::TypeError(String::New("Wrong number of arguments")));
		return scope.Close(Undefined());
	}
	/* �����`�F�b�N(�����̌^) */
	if (!args[0]->IsNumber() || !args[1]->IsFunction()) {
		ThrowException(Exception::TypeError(String::New("Wrong arguments")));
		return scope.Close(Undefined());
	}
	/* �����̏����L�� */
	recv_inf *inf = new recv_inf;
	inf->timeout = args[0]->Int32Value();
	inf->callback = Persistent<Function>::New(Local<Function>::Cast(args[1]));
	inf->result = 0;
	/* �o�b�N�O���E���h�Ŏ��s */
	uv_work_t *req_recv = new uv_work_t;
	req_recv->data = inf;
	uv_queue_work(uv_default_loop(), req_recv, _recvWait, _recvEnd);

	/* �Ƃ肠���������͖߂� */
	return scope.Close(Number::New(0));
}
//------------------------------------------------------------------------------
//[�֐�]�V���A���|�[�g�̐���M���̐ݒ�
//[����]�Ȃ�(���͐M���擾�݂̂̏ꍇ)
//[����]args[0] �M��(0:DTR,1:RTS)(Number)
//		args[1] �o��(0:OFF,1:ON)(Number)
//[�ߒl]0:	���͐M��(0x20:DSR,0x10:CTS)
//------------------------------------------------------------------------------
Handle<Value> serialCtrl(const Arguments& args) {
	HandleScope scope;

	/* COM�|�[�g�`�F�b�N */
	if (!bInitialized){
		ThrowException(Exception::TypeError(String::New("Not open")));
		return scope.Close(Undefined());
	}
	/* �����`�F�b�N(�����̐�) */
	if (args.Length() != 0 && args.Length() != 2){
		ThrowException(Exception::TypeError(String::New("Wrong number of arguments")));
		return scope.Close(Undefined());
	}
	if (args.Length() == 2){
		/* �����`�F�b�N(�����̌^) */
		if (!args[0]->IsNumber() || !args[1]->IsNumber()) {
			ThrowException(Exception::TypeError(String::New("Wrong arguments")));
			return scope.Close(Undefined());
		}
		int signal = args[0]->Int32Value();
		int value = args[1]->Int32Value();
		if (!(signal == 0 || signal == 1) || !(value == 0 || value == 1)){
			ThrowException(Exception::TypeError(String::New("Wrong arguments value")));
			return scope.Close(Undefined());
		}
		if (signal == 0){
			if (value == 0){
				EscapeCommFunction(hRs232c, CLRDTR);
			}
			else{
				EscapeCommFunction(hRs232c, SETDTR);
			}
		}
		else{
			if (value == 0){
				EscapeCommFunction(hRs232c, CLRRTS);
			}
			else{
				EscapeCommFunction(hRs232c, SETRTS);
			}
		}
	}
	/* �Ƃ肠���������͖߂� */
	DWORD signals;
	GetCommModemStatus(hRs232c, &signals);
	return scope.Close(Number::New(signals));
}
/* �����ɊO������Ă΂��֐��������Ă��� */
/* �O������Ă΂�閼�O�Ɠ����̊֐����̂Ђ��t�� */
void init(Handle<Object> exports) {
	exports->Set(String::NewSymbol("open"),
		FunctionTemplate::New(serialOpen)->GetFunction());
	exports->Set(String::NewSymbol("close"),
		FunctionTemplate::New(serialClose)->GetFunction());
	exports->Set(String::NewSymbol("send"),
		FunctionTemplate::New(serialSend)->GetFunction());
	exports->Set(String::NewSymbol("recv"),
		FunctionTemplate::New(serialRecv)->GetFunction());
	exports->Set(String::NewSymbol("ctrl"),
		FunctionTemplate::New(serialCtrl)->GetFunction());
}
/* ���W���[����require����鎞�ɌĂ΂�� */
NODE_MODULE(asyncSerial, init)
