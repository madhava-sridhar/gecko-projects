onmessage = function() {
  dump("GOT MESSAGE\n\n");
  self.clients.getAll().then(function(result) {
    for (i = 0; i < result.length; i++) {
      dump("POSTING MESSAGE\n\n");
      result[i].postMessage(i);
    }
  });
};
