var asyncSerial = require('./build/Debug/asyncSerial.node');

asyncSerial.open('COM3',9600,8,0,0);
asyncSerial.close();
asyncSerial.open('COM3',9600,8,0,0);

asyncSerial.send('0123456789',1000,function(result){
	console.log('send result = ' + result);
});
asyncSerial.recv(0,rcv);

function rcv(ret,txt)
{
	console.log('recv result = ' + ret);
	console.log('recv string = ' + txt);
	asyncSerial.recv(0,rcv);
}

