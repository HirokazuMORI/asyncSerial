var sio = require('./build/Release/asyncSerial.node');

sio.open('COM3',9600,8,0,0);

sio.recv(0,loopfunc);

function loopfunc(ret,txt)
{
	if(ret == 0){
		console.log(txt);
		sio.send(txt,1000,function(ret){});
	}
	sio.recv(0,loopfunc);
}
