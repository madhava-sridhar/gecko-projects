onmessage = function(e) {
  self.clients.getAll().then(function(res) {
    if (!res.length) {
      dump("ERROR: no clients are currently controlled.\n");
    }
    res[0].postMessage(e.data);
  });
};

