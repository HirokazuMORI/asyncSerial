#include <node.h>	/* 必須 */
#include <uv.h>		/* 非同期コールバックの為 */
using namespace v8;	/* 必須 */

#define SIZE_RCV_BUFFER	(1024)

/* スレッド間情報(送信) */
struct send_inf
{
	int timeout;					/* タイムアウト値 */
	Persistent<Function> callback;	/* 非同期処理完了コールバック関数 */
	int result;						/* 結果(待機した秒) */
	int length;						/* 送信文字数 */
	String::Utf8Value *buf;			/* 送信文字列 */
};
/* スレッド間情報(受信) */
struct recv_inf
{
	int timeout;					/* タイムアウト値 */
	Persistent<Function> callback;	/* 非同期処理完了コールバック関数 */
	int result;						/* 結果(待機した秒) */
	int length;						/* 受信文字数 */
	char buf[SIZE_RCV_BUFFER + 1];		/* 受信文字 */
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

/* バックグラウンド処理実行関数 */
void _sendWait(uv_work_t* req) {

	EnterCriticalSection(&secSend);	/* 二重送信処理防止 */
	send_inf* inf = static_cast<send_inf*>(req->data);	/* 情報引き継ぎ */
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
/* バックグラウンド処理完了関数 */
void _sendEnd(uv_work_t *req,int status) {
	HandleScope scope;

	send_inf* inf = static_cast<send_inf*>(req->data);	/* 情報引き継ぎ */
	Local<Value> argv[1] = { Number::New(inf->result) };		/* コールバック用引数設定 */
	inf->callback->Call(Context::GetCurrent()->Global(), 1, argv);
	/* 後処理 */
	delete inf;
	delete req;
}
/* バックグラウンド受信処理実行関数 */
void _recvWait(uv_work_t* req) {

	EnterCriticalSection(&secRecv);	/* 二重受信処理防止 */
	recv_inf* inf = static_cast<recv_inf*>(req->data);	/* 情報引き継ぎ */

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
				inf->result = RESULT_TIMEOUT;	/* タイムアウト */
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
/* バックグラウンド送信処理完了関数 */
void _recvEnd(uv_work_t *req, int status) {
	HandleScope scope;

	recv_inf* inf = static_cast<recv_inf*>(req->data);	/* 情報引き継ぎ */
	Local<Value> argv[3] = { 
		Number::New(inf->result),
		Number::New(inf->signal),
		String::New(inf->buf)
	};		/* コールバック用引数設定 */
	int argc = (inf->result == 0) ? 3 : 1;
	inf->callback->Call(Context::GetCurrent()->Global(), argc, argv);
	/* 後処理 */
	delete inf;
	delete req;
}
//------------------------------------------------------------------------------
//[関数]シリアルポートのオープン
//[引数]args[0]	COMポート名(string)
//		args[1] ボーレート(Number)
//		args[2] データビット長(Number)
//		args[3] ストップビット長(Number)
//		args[4] パリティ(Number)
//[戻値]0:	正常
//------------------------------------------------------------------------------
Handle<Value> serialOpen(const Arguments& args) {
	HandleScope scope;

	HANDLE hdl;

	/* 引数の数チェック */
	if(args.Length() != 5){
		ThrowException(Exception::TypeError(String::New("Wrong number of arguments")));
		return scope.Close(Undefined());
	}
	/* 引数の型チェック */
	if(!args[0]->IsString() ||
	   !args[1]->IsNumber() ||
	   !args[2]->IsNumber() ||
	   !args[3]->IsNumber() ||
	   !args[4]->IsNumber()){
		ThrowException(Exception::TypeError(String::New("Wrong arguments")));
		return scope.Close(Undefined());
	}
	/* 引数抽出 */
	String::Utf8Value strCom(args[0]);
	char *pcCom = *strCom;
	int iBoudRate = (int)args[1]->NumberValue();
	int iByteSize = (int)args[2]->NumberValue();
	int iStopBits = (int)args[3]->NumberValue();
	int iParity = (int)args[4]->NumberValue();

	/* ハンドル取得 */
	hdl = CreateFile(
        pcCom,GENERIC_READ|GENERIC_WRITE,0,NULL,
        OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED,NULL);
	if (hdl == INVALID_HANDLE_VALUE) {
		ThrowException(Exception::TypeError(String::New("CreateFile(INVALID_HANDLE_VALUE)")));
		return scope.Close(Undefined());
    }
	/* COMポートのセットアップ */
	if (!SetupComm(hdl, 1024, 1024)){
		ThrowException(Exception::TypeError(String::New("SetupComm(ERROR)")));
		return scope.Close(Undefined());
	}
	/* ポートの設定変更 */
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
	/* 制御信号OFF */
	EscapeCommFunction(hRs232c, CLRDTR);
	EscapeCommFunction(hRs232c, CLRRTS);
	return scope.Close(Number::New(0));
}
//------------------------------------------------------------------------------
//[関数]シリアルポートのクローズ
//[引数]なし
//[戻値]0:	正常
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
//[関数]シリアルポートの送信
//[引数]args[0]	送信文字列(string)
//		args[1] タイムアウト(ms)(Number)
//		args[2] 完了コールバック関数(Function)
//[戻値]0:	正常
//------------------------------------------------------------------------------
Handle<Value> serialSend(const Arguments& args) {
	HandleScope scope;

	/* COMポートチェック */
	if (!bInitialized){
		ThrowException(Exception::TypeError(String::New("Not open")));
		return scope.Close(Undefined());
	}
	/* 引数チェック(引数の数) */
	if (args.Length() != 3){
		ThrowException(Exception::TypeError(String::New("Wrong number of arguments")));
		return scope.Close(Undefined());
	}
	/* 引数チェック(引数の型) */
	if (!args[0]->IsString() || !args[1]->IsNumber() || !args[2]->IsFunction()) {
		ThrowException(Exception::TypeError(String::New("Wrong arguments")));
		return scope.Close(Undefined());
	}
	/* 引数の情報を記憶 */
	send_inf *inf = new send_inf;
	inf->buf = new String::Utf8Value(args[0]);
	inf->timeout = args[1]->Int32Value();
	inf->callback = Persistent<Function>::New(Local<Function>::Cast(args[2]));
	inf->result = 0;

	/* バックグラウンドで実行 */
	uv_work_t *req_send = new uv_work_t;
	req_send->data = inf;
	uv_queue_work(uv_default_loop(), req_send, _sendWait, _sendEnd);
	/* とりあえず処理は戻す */
	return scope.Close(Number::New(inf->buf->length()));
}
//------------------------------------------------------------------------------
//[関数]シリアルポートの受信
//[引数]args[0] タイムアウト(ms)(Number)
//		args[1] 完了コールバック関数(Function)
//[戻値]0:	正常
//------------------------------------------------------------------------------
Handle<Value> serialRecv(const Arguments& args) {
	HandleScope scope;

	/* COMポートチェック */
	if (!bInitialized){
		ThrowException(Exception::TypeError(String::New("Not open")));
		return scope.Close(Undefined());
	}
	/* 引数チェック(引数の数) */
	if (args.Length() != 2){
		ThrowException(Exception::TypeError(String::New("Wrong number of arguments")));
		return scope.Close(Undefined());
	}
	/* 引数チェック(引数の型) */
	if (!args[0]->IsNumber() || !args[1]->IsFunction()) {
		ThrowException(Exception::TypeError(String::New("Wrong arguments")));
		return scope.Close(Undefined());
	}
	/* 引数の情報を記憶 */
	recv_inf *inf = new recv_inf;
	inf->timeout = args[0]->Int32Value();
	inf->callback = Persistent<Function>::New(Local<Function>::Cast(args[1]));
	inf->result = 0;
	/* バックグラウンドで実行 */
	uv_work_t *req_recv = new uv_work_t;
	req_recv->data = inf;
	uv_queue_work(uv_default_loop(), req_recv, _recvWait, _recvEnd);

	/* とりあえず処理は戻す */
	return scope.Close(Number::New(0));
}
//------------------------------------------------------------------------------
//[関数]シリアルポートの制御信号の設定
//[引数]なし(入力信号取得のみの場合)
//[引数]args[0] 信号(0:DTR,1:RTS)(Number)
//		args[1] 出力(0:OFF,1:ON)(Number)
//[戻値]0:	入力信号(0x20:DSR,0x10:CTS)
//------------------------------------------------------------------------------
Handle<Value> serialCtrl(const Arguments& args) {
	HandleScope scope;

	/* COMポートチェック */
	if (!bInitialized){
		ThrowException(Exception::TypeError(String::New("Not open")));
		return scope.Close(Undefined());
	}
	/* 引数チェック(引数の数) */
	if (args.Length() != 0 && args.Length() != 2){
		ThrowException(Exception::TypeError(String::New("Wrong number of arguments")));
		return scope.Close(Undefined());
	}
	if (args.Length() == 2){
		/* 引数チェック(引数の型) */
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
	/* とりあえず処理は戻す */
	DWORD signals;
	GetCommModemStatus(hRs232c, &signals);
	return scope.Close(Number::New(signals));
}
/* ここに外部から呼ばれる関数を書いていく */
/* 外部から呼ばれる名前と内部の関数名のひも付け */
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
/* モジュールがrequireされる時に呼ばれる */
NODE_MODULE(asyncSerial, init)
