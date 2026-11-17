// The module's own error comes before a rejection nothing handled.
Promise.reject(new Error("unhandled"));
print("thrown");
throw new Error("own");
