create table test_vector(id Float32, vector Array(Float32), CONSTRAINT vector_len CHECK length(vector) = 3) engine MergeTree primary key id SETTINGS index_granularity=1024