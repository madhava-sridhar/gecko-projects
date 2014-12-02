function loop() {
  self.clients.getAll().then(function(result) {
    setTimeout(loop, 0);
  });
}

onactivate = function(e) {
  // spam getAll until the worker is closed.
  loop();
}

