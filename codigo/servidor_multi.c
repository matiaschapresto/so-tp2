#include <signal.h>
#include <errno.h>

#include "biblioteca.h"

/* Estructura que almacena los datos de una reserva. */
typedef struct {
	int posiciones[ANCHO_AULA][ALTO_AULA];
	int cantidad_de_personas;
	int rescatistas_disponibles;
} t_aula;


/* Estructura de parametros para los threads */
typedef struct {
	int t_socket;
	t_aula* aula;
} thread_args;


typedef struct {
	int fila;
	int columna;
} posicion;

/* mutex para sincronizar la actualizacion del aula */
pthread_mutex_t mutex_actualizar_aula;

/* mutex para los rescatistas */
pthread_mutex_t mutex_colocar_mascara;
pthread_mutex_t mutex_esperar_rescatista;
pthread_mutex_t** matriz_mutex_mover_alumno;

/* variables de condicion para los rescatistas y el grupo de alumnos */
pthread_cond_t condicion_hay_rescatistas;
pthread_cond_t condicion_desalojar_grupo_alumnos;
pthread_cond_t condicion_alumnos_dejaron_aula;
int cant_personas_con_mascara;
bool estan_los_5;
bool estan_saliendo_los_5;
int personas_que_salieron;
/********************************************************************************************************/


bool es_el_ultimo_grupo_del(t_aula* aula)
{
	return (aula->cantidad_de_personas <=5);
}


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


void t_aula_ingresar(t_aula* aula, t_persona* alumno)
{
	aula->cantidad_de_personas++;
	aula->posiciones[alumno->posicion_fila][alumno->posicion_columna]++;
}


void aula_ingresar_thread_safe(t_aula* aula, t_persona* alumno)
{
	pthread_mutex_lock(&mutex_actualizar_aula);
	t_aula_ingresar(aula, alumno);
	pthread_mutex_unlock(&mutex_actualizar_aula);
}


void t_aula_liberar(t_aula* aula, t_persona* alumno)
{
	aula->cantidad_de_personas--;
	cant_personas_con_mascara--;
	personas_que_salieron++;

	if (personas_que_salieron == 5) {
		personas_que_salieron = 0;
		estan_saliendo_los_5 = false;
		estan_los_5 = false;
		pthread_cond_broadcast(&condicion_alumnos_dejaron_aula);
	}
}


void aula_liberar_thread_safe(t_aula* aula, t_persona* alumno)
{
	pthread_mutex_lock(&mutex_actualizar_aula);
	t_aula_liberar(aula, alumno);
	pthread_mutex_unlock(&mutex_actualizar_aula);
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

	bool entre_limites = (fila >= 0) && (columna >= 0) &&
	     (fila < ANCHO_AULA) && (columna < ALTO_AULA);

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

	return pudo_moverse;
}


void calcular_orden_de_prioridades(posicion* posicion_menor_prioridad, posicion* posicion_mayor_prioridad, posicion* posicion_nueva, posicion* posicion_vieja)
{
	if ( (posicion_nueva->fila < posicion_vieja->fila) || 
		 (posicion_nueva->fila == posicion_vieja->fila && posicion_nueva->columna <= posicion_vieja->columna) ) 
	{
		posicion_mayor_prioridad->fila = posicion_nueva->fila;
		posicion_mayor_prioridad->columna = posicion_nueva->columna;
		posicion_menor_prioridad->fila = posicion_vieja->fila;
		posicion_menor_prioridad->columna = posicion_vieja->columna;
	} else {
		posicion_mayor_prioridad->fila = posicion_vieja->fila;
		posicion_mayor_prioridad->columna = posicion_vieja->columna;
		posicion_menor_prioridad->fila = posicion_nueva->fila;
		posicion_menor_prioridad->columna = posicion_nueva->columna;	
	}
}


t_comando intentar_moverse_thread_safe(t_aula* el_aula, t_persona* alumno, t_direccion dir)
{
	posicion posicion_nueva = {alumno->posicion_fila, alumno->posicion_columna};
	posicion posicion_vieja = {alumno->posicion_fila, alumno->posicion_columna};
	alumno->salio = direccion_moverse_hacia(dir, &posicion_nueva.fila, &posicion_nueva.columna);

	bool entre_limites = (posicion_nueva.fila >= 0) && (posicion_nueva.columna >= 0) &&
	     (posicion_nueva.fila < ANCHO_AULA) && (posicion_nueva.columna < ALTO_AULA);

	bool pudo_moverse = alumno->salio ||
	    (entre_limites && el_aula->posiciones[posicion_nueva.fila][posicion_nueva.columna] < MAXIMO_POR_POSICION); 

	posicion posicion_mayor_prioridad;
	posicion posicion_menor_prioridad;

	calcular_orden_de_prioridades(&posicion_menor_prioridad, &posicion_mayor_prioridad, &posicion_nueva, &posicion_vieja);

	if (pudo_moverse)
	{
		if (!alumno->salio) {
			pthread_mutex_lock(&matriz_mutex_mover_alumno[posicion_mayor_prioridad.fila][posicion_mayor_prioridad.columna]);
			pthread_mutex_lock(&matriz_mutex_mover_alumno[posicion_menor_prioridad.fila][posicion_menor_prioridad.columna]);
			el_aula->posiciones[posicion_nueva.fila][posicion_nueva.columna]++;
			el_aula->posiciones[posicion_vieja.fila][posicion_vieja.columna]--;
			pthread_mutex_unlock(&matriz_mutex_mover_alumno[posicion_menor_prioridad.fila][posicion_menor_prioridad.columna]);
			pthread_mutex_unlock(&matriz_mutex_mover_alumno[posicion_mayor_prioridad.fila][posicion_mayor_prioridad.columna]);
		} else {
			pthread_mutex_lock(&matriz_mutex_mover_alumno[posicion_menor_prioridad.fila][posicion_menor_prioridad.columna]);
			el_aula->posiciones[posicion_vieja.fila][posicion_vieja.columna]--;
			pthread_mutex_unlock(&matriz_mutex_mover_alumno[posicion_menor_prioridad.fila][posicion_menor_prioridad.columna]);
		}

		alumno->posicion_fila = posicion_nueva.fila;
		alumno->posicion_columna = posicion_nueva.columna;
	}

	return pudo_moverse;
}


void colocar_mascara(t_persona* alumno)
{
	printf("Esperando rescatista. Ya casi %s!\n", alumno->nombre);
	
	alumno->tiene_mascara = true;
	cant_personas_con_mascara++;

	if (cant_personas_con_mascara == 5){
		estan_los_5 = true;
		pthread_cond_broadcast(&condicion_desalojar_grupo_alumnos);
	}
}


void liberar_rescatista(t_aula* aula)
{
	pthread_mutex_lock(&mutex_esperar_rescatista);
	aula->rescatistas_disponibles++;
	pthread_mutex_unlock(&mutex_esperar_rescatista);
	pthread_cond_signal(&condicion_hay_rescatistas);
}



void colocar_mascara_thread_safe(t_persona* alumno, t_aula* aula)
{
	pthread_mutex_lock(&mutex_colocar_mascara);
	colocar_mascara(alumno);
	liberar_rescatista(aula);
	pthread_mutex_unlock(&mutex_colocar_mascara);
}


void esperar_rescatista_para(t_persona* alumno, t_aula* aula)
{
	pthread_mutex_lock(&mutex_esperar_rescatista);

	while (aula->rescatistas_disponibles == 0)
		pthread_cond_wait(&condicion_hay_rescatistas, &mutex_esperar_rescatista);

	aula->rescatistas_disponibles--;
	pthread_mutex_unlock(&mutex_esperar_rescatista);
}


void esperar_para_completar_grupo_de_5(t_persona* alumno, t_aula* aula)
{
	pthread_mutex_lock(&mutex_colocar_mascara);
	colocar_mascara(alumno);
	liberar_rescatista(aula);
		
	while (estan_saliendo_los_5) 
		pthread_cond_wait(&condicion_alumnos_dejaron_aula, &mutex_colocar_mascara);

	while (!estan_los_5)
		pthread_cond_wait(&condicion_desalojar_grupo_alumnos, &mutex_colocar_mascara);
	
	estan_saliendo_los_5 = true;

	pthread_mutex_unlock(&mutex_colocar_mascara);
}


void* atendedor_de_alumno(void* parameters)
{
	t_persona alumno;
	t_persona_inicializar(&alumno);
	thread_args* args = (thread_args*) parameters;
	t_aula* aula = args->aula;
	int t_socket = args->t_socket;

	if (recibir_nombre_y_posicion(t_socket, &alumno) != 0)
		terminar_servidor_de_alumno(t_socket, NULL, NULL);

	printf("Atendiendo a %s en la posicion (%d, %d), tiene el socket %d\n",
			alumno.nombre, alumno.posicion_fila, alumno.posicion_columna, t_socket);

	aula_ingresar_thread_safe(aula, &alumno);

	// Loop de espera de pedido de movimiento.
	for(;;) {
		t_direccion direccion;

		// Esperamos un pedido de movimiento.
		if (recibir_direccion(t_socket, &direccion) != 0)
			terminar_servidor_de_alumno(t_socket, aula, &alumno);

		// Tratamos de movernos en nuestro modelo
		bool pudo_moverse = intentar_moverse_thread_safe(aula, &alumno, direccion);

		if (pudo_moverse)
			printf("%s se movio a: (%d, %d)\n", alumno.nombre, alumno.posicion_fila, alumno.posicion_columna);
		else
			printf("No se pudo mover a: %s\n", alumno.nombre);

		// Avisamos que ocurrio
		enviar_respuesta(t_socket, pudo_moverse ? OK : OCUPADO);
		if (alumno.salio)
			break;
	}

	esperar_rescatista_para(&alumno, aula);
    /* Son menos de 5 en el aula. Les coloco la mascara y salen directamente. */
	if (es_el_ultimo_grupo_del(aula)) 
		colocar_mascara_thread_safe(&alumno, aula);	
	else
		esperar_para_completar_grupo_de_5(&alumno, aula); //Les coloco la mascara pero se quedan esperando.

	aula_liberar_thread_safe(aula, &alumno);
	enviar_respuesta(t_socket, LIBRE);

	printf("Listo, %s es libre!\n", alumno.nombre);

	pthread_exit(NULL);
}


int main(void)
{
	int socketfd_cliente, socket_servidor, socket_size;
	struct sockaddr_in local, remoto;

    /* inicializamos los mutex y variables de condicion */	
	matriz_mutex_mover_alumno = malloc(ALTO_AULA * sizeof(pthread_mutex_t*));
	int i, j;
	for(i = 0; i < ALTO_AULA; i++) {
		matriz_mutex_mover_alumno[i] = malloc(ANCHO_AULA * sizeof(pthread_mutex_t));
		for (j = 0; j < ANCHO_AULA; j++) {
			pthread_mutex_init(&matriz_mutex_mover_alumno[i][j], NULL);
		}
	}

	pthread_mutex_init(&mutex_actualizar_aula, NULL);
	pthread_mutex_init(&mutex_colocar_mascara, NULL);
	pthread_mutex_init(&mutex_esperar_rescatista, NULL);

    /* Inicio las variables de condicion */	
	pthread_cond_init(&condicion_hay_rescatistas, NULL);
	pthread_cond_init(&condicion_desalojar_grupo_alumnos, NULL);
	pthread_cond_init(&condicion_alumnos_dejaron_aula, NULL);
	cant_personas_con_mascara = 0;
	personas_que_salieron = 0;
	estan_los_5 = false;
	estan_saliendo_los_5 = false;

	/* Crear un socket de tipo INET con TCP (SOCK_STREAM). */
	if ((socket_servidor = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("creando socket");
	}

	/* Crear nombre, usamos INADDR_ANY para indicar que cualquiera puede conectarse aqui. */
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
	for(;;) {
		if ((socketfd_cliente = accept(socket_servidor, (struct sockaddr*) &remoto,
                                       (socklen_t*) &socket_size)) == -1) {
			printf("!! Error al aceptar conexion\n");
		} else {
			// Creo un thread por cada cliente que quiere conectarse
			pthread_t thread;
			thread_args* args = malloc(sizeof(thread_args));
			args->t_socket = socketfd_cliente;
			args->aula = &el_aula;
			if (pthread_create(&thread, NULL, atendedor_de_alumno, (void*) args) != 0) {
				printf("Error al crear thread!!");
				return -1;
			}
		}
	}

	pthread_mutex_destroy(&mutex_actualizar_aula);
	pthread_mutex_destroy(&mutex_colocar_mascara);
	pthread_mutex_destroy(&mutex_esperar_rescatista);
	pthread_cond_destroy(&condicion_hay_rescatistas);
	pthread_cond_destroy(&condicion_desalojar_grupo_alumnos);

	pthread_exit(NULL);
}
