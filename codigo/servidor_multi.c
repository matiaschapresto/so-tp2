/*
 * servidor_multi.c
 *
 *  Created on: Nov 2, 2014
 *      Author: storres
 */


#include <signal.h>
#include <errno.h>

#include "biblioteca.h"

/* Estructura que almacena los datos de una reserva. */
typedef struct {
	int posiciones[ANCHO_AULA][ALTO_AULA];
	int cantidad_de_personas;

	int rescatistas_disponibles;
} t_aula;


typedef struct {
	int t_socket;
	t_aula *aula;
} thread_args;


void t_aula_iniciar_vacia(t_aula *un_aula)
{
	int i, j;
	for(i = 0; i < ANCHO_AULA; i++)
	{
		for (j = 0; j < ALTO_AULA; j++)
		{
			un_aula->posiciones[i][j] = 0;
		}
	}

	un_aula->cantidad_de_personas = 0;

	un_aula->rescatistas_disponibles = RESCATISTAS;
}

void t_aula_ingresar(t_aula *un_aula, t_persona *alumno)
{
	un_aula->cantidad_de_personas++;
	un_aula->posiciones[alumno->posicion_fila][alumno->posicion_columna]++;
}

void t_aula_liberar(t_aula *un_aula, t_persona *alumno)
{
	un_aula->cantidad_de_personas--;
	un_aula->posiciones[alumno->posicion_fila][alumno->posicion_columna]--;
}

static void terminar_servidor_de_alumno(int socket_fd, t_aula *aula, t_persona *alumno) {
	printf(">> Se interrumpió la comunicación con una consola.\n");

	close(socket_fd);

	t_aula_liberar(aula, alumno);
	exit(-1);
}


t_comando intentar_moverse(t_aula *el_aula, t_persona *alumno, t_direccion dir)
{
	int fila = alumno->posicion_fila;
	int columna = alumno->posicion_columna;
	alumno->salio = direccion_moverse_hacia(dir, &fila, &columna);

	///char buf[STRING_MAXIMO];
	///t_direccion_convertir_a_string(dir, buf);
	///printf("%s intenta moverse hacia %s (%d, %d)... ", alumno->nombre, buf, fila, columna);


	bool entre_limites = (fila >= 0) && (columna >= 0) &&
	     (fila < ALTO_AULA) && (columna < ANCHO_AULA);

	bool pudo_moverse = alumno->salio ||
	    (entre_limites && el_aula->posiciones[fila][columna] < MAXIMO_POR_POSICION);


	if (pudo_moverse)
	{
		if (!alumno->salio)
			el_aula->posiciones[fila][columna]++;
		el_aula->posiciones[alumno->posicion_fila][alumno->posicion_columna]--;
		alumno->posicion_fila = fila;
		alumno->posicion_columna = columna;
	}


	//~ if (pudo_moverse)
		//~ printf("OK!\n");
	//~ else
		//~ printf("Ocupado!\n");


	return pudo_moverse;
}

void colocar_mascara(t_aula *el_aula, t_persona *alumno)
{
	printf("Esperando rescatista. Ya casi %s!\n", alumno->nombre);

	alumno->tiene_mascara = true;
}


void *atendedor_de_alumno(void *parameters)
{
	t_persona alumno;
	t_persona_inicializar(&alumno);
	thread_args *args = parameters;
	if (recibir_nombre_y_posicion(args->t_socket, &alumno) != 0) {
		/* O la consola cortó la comunicación, o hubo un error. Cerramos todo. */
		terminar_servidor_de_alumno(args->t_socket, NULL, NULL);
	}

	printf("Atendiendo a %s en la posicion (%d, %d)\n",
			alumno.nombre, alumno.posicion_fila, alumno.posicion_columna);

	t_aula_ingresar(args->aula, &alumno);

	/// Loop de espera de pedido de movimiento.
	for(;;) {
		t_direccion direccion;

		/// Esperamos un pedido de movimiento.
		if (recibir_direccion(args->t_socket, &direccion) != 0) {
			/* O la consola cortó la comunicación, o hubo un error. Cerramos todo. */
			terminar_servidor_de_alumno(args->t_socket, args->aula, &alumno);
		}

		/// Tratamos de movernos en nuestro modelo
		bool pudo_moverse = intentar_moverse(args->aula, &alumno, direccion);

		printf("%s se movio a: (%d, %d)\n", alumno.nombre, alumno.posicion_fila, alumno.posicion_columna);

		/// Avisamos que ocurrio
		enviar_respuesta(args->t_socket, pudo_moverse ? OK : OCUPADO);
		//printf("aca3\n");

		if (alumno.salio)
			break;
	}

	colocar_mascara(args->aula, &alumno);

	t_aula_liberar(args->aula, &alumno);
	enviar_respuesta(args->t_socket, LIBRE);

	printf("Listo, %s es libre!\n", alumno.nombre);

	return NULL;

}


int main(void)
{
	//signal(SIGUSR1, signal_terminar);
	int socketfd_cliente, socket_servidor, socket_size;
	struct sockaddr_in local, remoto;



	/* Crear un socket de tipo INET con TCP (SOCK_STREAM). */
	if ((socket_servidor = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("creando socket");
	}

	/* Crear nombre, usamos INADDR_ANY para indicar que cualquiera puede conectarse aquí. */
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = INADDR_ANY;
	local.sin_port = htons(PORT);

	if (bind(socket_servidor, (struct sockaddr *)&local, sizeof(local)) == -1) {
		perror("haciendo bind");
	}

	/* Escuchar en el socket y permitir 5 conexiones en espera. */
	if (listen(socket_servidor, 5) == -1) {
		perror("escuchando");
	}

	t_aula el_aula;
	t_aula_iniciar_vacia(&el_aula);

	/// Aceptar conexiones entrantes.
	socket_size = sizeof(remoto);
	for(;;){
		if (-1 == (socketfd_cliente =
					accept(socket_servidor, (struct sockaddr*) &remoto, (socklen_t*) &socket_size)))
		{
			printf("!! Error al aceptar conexion\n");
		}
		else {
			// Creo un thread por cada cliente que quiere conectarse
			pthread_t thread;
			thread_args args = {socketfd_cliente, &el_aula};
			if (pthread_create(&thread, NULL, &atendedor_de_alumno, &args) != 0) {
				//atendedor_de_alumno(socketfd_cliente, &el_aula);
				printf("Error al crear thread!!");
				return -1;
			}
		}
	}


	return 0;
}
