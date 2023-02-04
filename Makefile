all:
	g++ main.cpp send_to_clients.cpp receive_from_clients.cpp calculate_fights.cpp -o server -pthread

debug:
	
	g++ main.cpp send_to_clients.cpp receive_from_clients.cpp calculate_fights.cpp -g -o server -pthread
clean:
	rm server
