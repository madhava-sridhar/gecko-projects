function ok(a, msg) {
  postMessage({ type: 'check', check: !!a, message: msg });
}

function is(a, b, msg) {
  ok (a === b, msg);
}

function finish() {
  postMessage({ type: 'finish' });
}

function basic()
{
  var a = new MessageChannel();
  ok(a, "MessageChannel created");

  var port1 = a.port1;
  ok(port1, "MessageChannel.port1 exists");
  is(port1, a.port1, "MessageChannel.port1 is port1");

  var port2 = a.port2;
  ok(port2, "MessageChannel.port1 exists");
  is(port2, a.port2, "MessageChannel.port2 is port2");

  [ 'postMessage', 'start', 'close' ].forEach(function(e) {
    ok(e in port1, "MessagePort1." + e + " exists");
    ok(e in port2, "MessagePort2." + e + " exists");
  });

  runTests();
}

function sendMessages()
{
  var a = new MessageChannel();
  ok(a, "MessageChannel created");

  a.port1.postMessage("Hello world!");
  a.port1.onmessage = function(e) {
    is(e.data, "Hello world!", "The message is back!");
    runTests();
  }

  a.port2.onmessage = function(e) {
    a.port2.postMessage(e.data);
  }
}

var tests = [
  basic,
  sendMessages,
];

function runTests() {
  if (!tests.length) {
    finish();
    return;
  }

  var t = tests.shift();
  t();
}

onmessage = function() {
  runTests();
}
