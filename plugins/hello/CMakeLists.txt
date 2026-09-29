# readyup-hello: example plugin (-> build/plugins/hello.so). Not part of release zips.
readyup_add_plugin(hello hello.c)
target_link_libraries(readyup_plugin_hello PRIVATE m)  # hello_nade (cos/sin)
